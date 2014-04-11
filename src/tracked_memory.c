
/**
 * @file	tracked_memory.c
 * @author	James Warren
 * @copyright	James Warren, 2013-2014
 * @license	Zlib (see license.txt or http://opensource.org/licenses/Zlib)
 */


#include "tracked_memory.h"		// prototypes, definitions

// This file is only valid if USING_MEMORY_DEBUGGING is enabled
#if defined(USING_MEMORY_DEBUGGING)

#include <stdio.h>			// printf, stdout
#include <stdlib.h>			// malloc, free

#if defined(__linux__) || defined(BSD)
#	include <string.h>		// memcmp, memset, memmove
#endif



// definitions that can be replaced or implemented elsewhere
#define MAX_LEN_GENERIC		250

// Magic values, assigned and checked with memory operations
#define MEM_HEADER_MAGIC	0xCAFEFACE
#define MEM_FOOTER_MAGIC	0xDEADBEEF
// Memory-fill values, used before and after alloc/free
#define MEM_ON_INIT		0x0F
#define MEM_AFTER_FREE		0xFF


#define block_offset_header(real_mem)           \
		(struct memblock_header*)((uint8_t*)real_mem - sizeof(struct memblock_header))
#define block_offset_realmem(memblock)          \
		(void*)((uint8_t*)memblock + sizeof(struct memblock_header))
#define block_offset_footer(memblock, num_bytes)\
		(struct memblock_footer*)((uint8_t*)memblock + (sizeof(struct memblock_header) + num_bytes))
#define HEADER_FOOTER_SIZE			\
		(sizeof(struct memblock_header) + sizeof(struct memblock_footer))


// usage as variables allow them to be easily inserted into memcmp's
const unsigned	mem_header_magic = MEM_HEADER_MAGIC;
const unsigned	mem_footer_magic = MEM_FOOTER_MAGIC;


struct mem_context		g_mem_ctx;



void
mem_context_init(
	struct mem_context* const context
)
{
#if defined(_WIN32)
	InitializeCriticalSection(&context->cs);
#else
	pthread_mutexattr_settype(&context->lock_attrib, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutexattr_settype(&context->lock_attrib, PTHREAD_PROCESS_PRIVATE);
	pthread_mutex_init(&context->lock, &context->lock_attrib);
#endif
	
	context->allocs 		= 0;
	context->frees			= 0;
	context->current_allocated	= 0;
	context->total_allocated	= 0;
	
	TAILQ_INIT(&context->memblocks);
}



void
mem_context_destroy(
	struct mem_context* const context
)
{
	if ( !TAILQ_EMPTY(&context->memblocks) )
	{
		printf("Memory Leak Detected\n\nCheck '%s' for details\n",
		       MEM_LEAK_LOG_NAME);
	}

	output_memory_info(context);

#if defined(_WIN32)
	DeleteCriticalSection(&context->cs);
#else
	pthread_mutex_destroy(&context->lock);
#endif
}



enum E_MEMORY_ERROR
check_block(
	struct memblock_header* memory_block
)
{
	uint32_t	block_size = 0;

	if ( memory_block == NULL )
		goto null_block;

#if !defined(DISABLE_MEMORY_CHECK_TO_STDOUT)
	printf("\tChecking Memory Block %p..\n",
		memory_block);
	printf("\t\tGlobal Header magic number is %lu bytes in size :: %#x\n",
		sizeof(mem_header_magic), mem_header_magic);
	printf("\t\tGlobal Footer magic number is %lu bytes in size :: %#x\n",
		sizeof(mem_footer_magic), mem_footer_magic);
#endif

	if ( memcmp(&memory_block->magic,
		&mem_header_magic,
		sizeof(mem_header_magic)) != 0 )
	{
		/* memory_block magic number has been modified; block contents
		 * are unstable */
		goto corrupt_header;
	}

#if !defined(DISABLE_MEMORY_CHECK_TO_STDOUT)
	printf("\t\tHas a valid header (%lu bytes, %#x)...\n",
		sizeof(memory_block->magic), memory_block->magic);
	printf("\t\tHeader Info:: %u (%u requested) bytes, line %u in %s\n",
		memory_block->real_size, memory_block->requested_size,
		memory_block->line, memory_block->file);
#endif

	if ( memory_block->footer == NULL ||
		memcmp(&memory_block->footer->magic, &mem_footer_magic,	sizeof(mem_footer_magic)) != 0 )
	{
		/* memory_block footer magic has been modified - as the header
		 * is not corrupt, we can retrieve the allocation info safely */
		goto corrupt_footer;
	}

#if !defined(DISABLE_MEMORY_CHECK_TO_STDOUT)
	printf("\t\tHas a valid footer (%lu bytes, %#x)...\n",
		sizeof(memory_block->footer->magic),
		memory_block->footer->magic);
#endif

	// calculate the size requested by removing the header + footer
	block_size = ((uint8_t*)memory_block->footer) - ((uint8_t*)memory_block + sizeof(struct memblock_header));

#if !defined(DISABLE_MEMORY_CHECK_TO_STDOUT)
	printf("\t\tCalculated block_size is %u bytes\n", block_size);
#endif

	if ( memory_block->requested_size != block_size )
	{
		/* The size stored by the memory_block header is different from
		 * that calculated (remember we've already validated the magic
		 * numbers) */
		goto invalid_size;
	}

#if !defined(DISABLE_MEMORY_CHECK_TO_STDOUT)
	printf("\t\tHas a valid app_mem size (%u bytes)\n\t\tValidated!\n",
	       block_size);
#endif

	// memory_block is as expected

	return EC_NoError;

null_block:
	// Optional: Raise error
	return EC_NoMemoryBlock;
corrupt_header:
	// Optional: Raise error
	return EC_CorruptHeader;
corrupt_footer:
	// Optional: Raise error
	return EC_CorruptFooter;
invalid_size:
	// Optional: Raise error
	return EC_SizeMismatch;
}



void
output_memory_info(
	struct mem_context* const context
)
{
	enum E_MEMORY_ERROR	result;
	struct memblock_header*	block_ptr;
	FILE*		leak_file;
	int32_t		close_file = 1;
	uint32_t	i = 0;
	uint32_t	j;
	// we don't store/track the user-requested amounts, only the real
	uint32_t	requested_alloc;
	uint32_t	requested_unfreed;

	/* since Windows has been kind enough to not provide more standards
	 * complaint security functionality, we shall have to use their own
	 * fopen_s. POSIX builds can happily use the 'x' flag to provide a
	 * decent security level - fopen_s is still vulnerable to potential
	 * exploits ironically... */
#if defined(_WIN32)
	if ( fopen_s(&leak_file, MEM_LEAK_LOG_NAME, "w+") != 0 )
	{
#else
	if (( leak_file = fopen(MEM_LEAK_LOG_NAME, "w+")) == NULL )
	{
#endif
		leak_file = stdout;
		close_file = 0;
	}

	/* Remove memory block sizes, multiplied by the number of allocations,
	 * which is taken away from the total amount allocated. */
	requested_alloc		= context->total_allocated - (HEADER_FOOTER_SIZE * context->allocs);
	/* Remove memory block sizes, multiplied by the number of pending frees,
	 * which is to be taken away from the current amount still allocated. */
	requested_unfreed	= context->current_allocated - (HEADER_FOOTER_SIZE * (context->allocs - context->frees));

	fprintf(leak_file,
		"# Details\n"
		"Header+Footer Size......: %lu\n"
		"\n"
		"# Code Stats\n"
		"Allocations.............: %u\n"
		"Frees...................: %u\n"
		"Pending Frees...........: %u\n"
		"\n"
		"# Totals, Real\n"
		"Bytes Allocated.........: %u\n"
		"Unfreed Bytes...........: %u\n"
		"\n"
		"# Totals, Requested\n"
		"Bytes Allocated.........: %u\n"
		"Unfreed Bytes...........: %u\n"
		"\n"
		"##################\n"
		"  Unfreed Blocks  \n",
		HEADER_FOOTER_SIZE,
		context->allocs, context->frees, (context->allocs - context->frees),
		context->total_allocated, context->current_allocated,
		requested_alloc, requested_unfreed
	);

	TAILQ_FOREACH(block_ptr, &context->memblocks, np_blocks)
	{
		i++;

		fprintf(leak_file,
			"##################\n"
			"%u)\n"
			"Block...: " PRINT_POINTER
			"\n",
			i, (uintptr_t)block_ptr
		);

		result = check_block(block_ptr);
		switch ( result )
		{
		case EC_NoMemoryBlock:
			{
				fprintf(leak_file,
					"Error...: Block Pointer was NULL\n"
				);
				break;
			}
		case EC_CorruptFooter:
			{
				fprintf(leak_file,
					"Error...: Corrupt Footer\n"
				);
				break;
			}
		case EC_CorruptHeader:
			{
				fprintf(leak_file,
					"Error...: Corrupt Header\n"
				);
				break;
			}
		case EC_SizeMismatch:
			{
				fprintf(leak_file,
					"Error...: Size Mismatch (%u actual bytes)\n",
					block_ptr->requested_size
				);
				break;
			}
		case EC_NoError:
		default:
			break;
		}

		/* can't fall through in switch, so have to do a secondary check
		 * as we can't print data that's corrupt or a NULL */
		if ( result != EC_NoMemoryBlock && result != EC_CorruptHeader )
		{
			fprintf(leak_file,
				"Size....: %u\n"
				"Function: %s\n"
				"File....: %s\n"
				"Line....: %u\n",
				block_ptr->requested_size,
				block_ptr->function,
				block_ptr->file,
				block_ptr->line
			);
			fprintf(leak_file, "Data....: ");
			for (   j = 0;
				j < block_ptr->requested_size && j < MEM_OUTPUT_LIMIT;
				j++ )
			{
				/** @todo causes warning 'cast to pointer from
				 * integer of different size' and 'format %x
				 * expects argument of type unsigned int' */
				fprintf(leak_file,
					"%02x ",
					block_offset_realmem(block_ptr)[j]);
			}
			fprintf(leak_file, "\n");
		}
	}

	if ( close_file )
		fclose(leak_file);

	/* try to free whatever we didn't during runtime; if any of these are
	 * screwed (heap corruption) then this will probably trigger a crash */
	TAILQ_FOREACH(block_ptr, &context->memblocks, np_blocks)
	{
		TAILQ_REMOVE(&context->memblocks, block_ptr, np_blocks);
		free(block_ptr);
	}
}



void*
tracked_alloc(
	struct mem_context* const context,
	const uint32_t num_bytes,
	const char* file,
	const char* function,
	const uint32_t line
)
{
	struct memblock_header*	mem_block = NULL;
	struct memblock_footer*	mem_footer = NULL;
	void*			mem_return = NULL;
	char*			p = NULL;
	uint32_t		patched_alloc = 0;	// num_bytes + memblocks

	// allocate the requested amount, plus the size of the header & footer memblocks
	patched_alloc = num_bytes + HEADER_FOOTER_SIZE;

	// the actual, real, physical allocation of memory
	mem_block = (struct memblock_header*)malloc(patched_alloc);

	if ( mem_block == NULL )
		goto alloc_failure;

	// initialize the value for the new memory
	memset(mem_block, MEM_ON_INIT, patched_alloc);

#if defined(_WIN32)
#	define PATH_CHAR	'\\'
#else
#	define PATH_CHAR	'/'
#endif
	// we don't want the full path information that compilers set
	if (( p = (char*)strrchr(file, PATH_CHAR)) != NULL )
		file = ++p;

	// calculate the offsets of the return memory and the footer
	mem_return = block_offset_realmem(mem_block);
	mem_footer = block_offset_footer(mem_block, num_bytes);

	// prepare the structure internals
	mem_footer->magic	= mem_footer_magic;
	mem_block->footer	= mem_footer;
	mem_block->magic	= mem_header_magic;
	mem_block->line		= line;
	mem_block->real_size		= patched_alloc;
	mem_block->requested_size	= num_bytes;

	/* you should be using strlcpy if you're doing character buffer copying
	 * and other functionality!! For the sake of securing your application,
	 * please USE IT!
	 * We provide a 'normal, well-known' alternative for the sake of the
	 * example using strncpy (which is in standard headers everywhere) */
#if defined(HAVE_STRLCPY)
	strlcpy(mem_block->file, file, sizeof(mem_block->file));
	strlcpy(mem_block->function, function, sizeof(mem_block->function));
#else
	strncpy(mem_block->file, file, sizeof(mem_block->file)-1);
	strncpy(mem_block->function, function, sizeof(mem_block->function)-1);
#endif

	/* lock this context, only 1 thread to update sensitive internals at a
	 * time - lock for as little time as possible! */
#if defined(_WIN32)
	EnterCriticalSection(&context->cs);
#else
	pthread_mutex_lock(&context->lock);
#endif

	// update the stats, using patched values
	context->allocs++;
	context->current_allocated += patched_alloc;
	context->total_allocated += patched_alloc;
	// append it to the list
	TAILQ_INSERT_TAIL(&context->memblocks, mem_block, np_blocks);

	// unlock the context, other threads can now allocate from this class
#if defined(_WIN32)
	LeaveCriticalSection(&context->cs);
#else
	pthread_mutex_unlock(&context->lock);
#endif

	return mem_return;

alloc_failure:
	return NULL;
}



void
tracked_free(
	struct mem_context* const context,
	void* memory
)
{
	struct memblock_header*	mem_block = NULL;

	// as per the C standard, if it's a NULL, do nothing
	if ( memory == NULL )
		return;

	if ( !validate_memory(context, memory) )
		return;

	mem_block = block_offset_header(memory);

#if !defined(DISABLE_MEMORY_OP_TO_STDOUT)
	printf( "free [%s (%u bytes) line %u]\n"
		"\tBlock: %p | Usable Block: %p\n",
		mem_block->file, mem_block->requested_size, mem_block->line,
		mem_block, memory);
#endif

	// stop other modifications on this memory context
#if defined(_WIN32)
	EnterCriticalSection(&context->cs);
#else
	pthread_mutex_lock(&context->lock);
#endif

	// update the context stats
	context->frees++;
	context->current_allocated -= (mem_block->real_size);
	// remove the mem_block from the list
	TAILQ_REMOVE(&context->memblocks, mem_block, np_blocks);

	// we're done with the class internals, open it up again
#if defined(_WIN32)
	LeaveCriticalSection(&context->cs);
#else
	pthread_mutex_unlock(&context->lock);
#endif

	// fill the app-allocated memory (highlights use after free)
	memset(mem_block, MEM_AFTER_FREE, mem_block->real_size);
	// perform the actual freeing of memory, including our header + footer
	free(mem_block);

	return;
}



void*
tracked_realloc(
	struct mem_context* const context,
	void* memory,
	const uint32_t new_num_bytes,
	const char* file,
	const char* function,
	const uint32_t line
)
{
	struct memblock_header*	mem_block = NULL;
	void*			mem_return = NULL;

	if ( memory == NULL )
	{
		// if the memory is NULL, call malloc [ISO C]
		return tracked_alloc(context, new_num_bytes, file, function, line);
	}

	if ( new_num_bytes == 0 )
	{
		/* if new_num_bytes is 0 and the memory is not null, call
		 * free() [ISO C] */
		tracked_free(context, memory);
		/* whether it works or fails, the memory is still unusable, so
		 * return a NULL */
		return NULL;
	}


#if defined(_WIN32)
	EnterCriticalSection(&context->cs);
#else
	pthread_mutex_lock(&context->lock);
#endif


#if !defined(DISABLE_MEMORY_OP_TO_STDOUT)
	// since we call TrackedAlloc, make the log info accurate
	printf( "realloc [%s (%u bytes) line %u]\n"
		"\tTo be allocated in the following malloc; using memory at: %p\n",
		file, new_num_bytes, line,
		memory);
#endif

	mem_return = (void*)tracked_alloc(context, new_num_bytes, file, function, line);

	if ( mem_return != NULL )
	{
		mem_block = block_offset_header(memory);

		// move the original data into the new allocation
		mem_block->requested_size < new_num_bytes ?
			memmove(mem_return, memory, new_num_bytes) :
			memmove(mem_return, memory, mem_block->requested_size);

		// free the original block
		tracked_free(context, memory);
	}


#if defined(_WIN32)
	LeaveCriticalSection(&context->cs);
#else
	pthread_mutex_unlock(&context->lock);
#endif

	return mem_return;
}



bool
validate_memory(
	struct mem_context* const context,
	void* memory
)
{
	bool	ret = true;
	struct memblock_header*	mem_block;

#if defined(_WIN32)
	EnterCriticalSection(&context->cs);
#else
	pthread_mutex_lock(&context->lock);
#endif

	// if no pointer was specified, check the entire list
	if ( memory == NULL )
	{
		TAILQ_FOREACH(mem_block, &context->memblocks, np_blocks)
		{
			// bail if a block is invalid
			if ( check_block(mem_block) != EC_NoError )
			{
				ret = false;
				break;
			}
		}
	}
	else
	{
		mem_block = block_offset_header(memory);

		ret = (check_block(mem_block) == EC_NoError);
	}

#if defined(_WIN32)
	EnterCriticalSection(&context->cs);
#else
	pthread_mutex_unlock(&context->lock);
#endif

	return ret;
}



#endif	// USING_MEMORY_DEBUGGING
