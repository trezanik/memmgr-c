#ifndef TRACKED_MEMORY_H_INCLUDED
#define TRACKED_MEMORY_H_INCLUDED

/**
 * @file	tracked_memory.h
 * @author	James Warren
 * @brief	Memory Allocation tracking for the application
 */


/* bring in this definition from a 'build.h' or ./configure setting. For now, we
 * specify it here. Adjust for your own project as necessary. We also define the
 * other definitions also needed */
#define USING_MEMORY_DEBUGGING
#define DISABLE_MEMORY_CHECK_TO_STDOUT	// no spam - toggle on/off
#define DISABLE_MEMORY_OP_TO_STDOUT


#if defined(USING_MEMORY_DEBUGGING)

#	if defined(_WIN32)
#		include "queue.h"		// sys/queue.h Win32 implementation
#		define _WIN32_LEAN_AND_MEAN	// yeah, right
#		include <Windows.h>		// CriticalSection
#		include <stdint.h>		// data types
		// stupid windows.. this is terrible, but quick!
#		define bool int32_t
#		define true 1
#		define false 0
#	else
		// sys/queue confirmed ok on linux & freebsd
#		include <sys/queue.h>		// Native linked-list macros
#		include <pthread.h>		// pthread_mutex
#		define __STDC_FORMAT_MACROS
#		include <inttypes.h>		// for PRIxPTR
#		include <stdbool.h>		// C99 supplies bool
#	endif


// required definitions
#define MEM_LEAK_LOG_NAME		"memdynamic.log"
#define MEM_MAX_FILENAME_LENGTH		31
#define MEM_MAX_FUNCTION_LENGTH		31

/* don't ask. I did this a while ago and had issues (I believe it was with
 * visual studio, as usual), this is what I came up with for a workaround, and
 * it works, so... */
#if defined(_WIN64)
#	define PRINT_POINTER	"%016p"
#elif defined(_WIN32)
#	define PRINT_POINTER	"%08p"
#elif defined(__x86_64__)
#	define PRINT_POINTER	"%016" PRIxPTR " "
#else
#	define PRINT_POINTER	"%08" PRIxPTR
#endif



/**
 * This structure is added to the end of an allocated block of memory, when
 * USING_MEMORY_DEBUGGING is defined.
 *
 * This ensures no data has been written beyond the boundary of an allocated bit
 * of memory, resulting in heap/stack corruption.
 *
 * @struct memblock_footer
 */
struct memblock_footer
{
	unsigned	magic;		/**< footer magic number */
};


/**
 * This structure is added at the start of a block of allocated memory, when
 * USING_MEMORY_DEBUGGING is defined.
 *
 * @struct memblock_header
 */
struct memblock_header
{
	/**
	 * The header magic number is used to detect if an operation on memory has
	 * written into this structure. Corrupt headers usually mean something else
	 * has written into the block - an exception being if an operation has
	 * stepped too far back, to the extent of overwriting the magic number */
	unsigned		magic;

	/** A pointer to this memory blocks footer */
	struct memblock_footer*	footer;

	/**
	 * The file this memory block was created in; is not allocated dynamically,
	 * and so is bound by the MEM_MAX_FILENAME_LENGTH definition.
	 */
	char		file[MEM_MAX_FILENAME_LENGTH+1];
	/** The function name this memory block was created in; like the file
	 * member, is not allocated dynamically, and so is bound by the
	 * MEM_MAX_FUNCTION_LENGTH definition */
	char		function[MEM_MAX_FUNCTION_LENGTH+1];
	/** The line in the file this memory block was created in */
	uint32_t	line;
	/** The size, in bytes, the original request desired */
	uint32_t	requested_size;
	/** The size, in bytes, of the total allocation (header+data+footer) */
	uint32_t	real_size;
	/** Linked list entry */
	TAILQ_ENTRY(memblock_header)	np_blocks;
};



/**
 * Memory-specific error codes. Used only as the return value from check_block().
 *
 * @enum E_MEMORY_ERROR
 */
enum E_MEMORY_ERROR
{
	EC_NoError = 0,
	EC_NoMemoryBlock,
	EC_CorruptHeader,
	EC_CorruptFooter,
	EC_SizeMismatch
};



/**
 * Holds the stats and pointers to the memory operations performed. You can
 * create multiple contexts in an application if desired, so you can separate
 * tracking between network, gui, etc.
 *
 * By default, there's a single, global context used, which is automatically
 * passed in by the custom macros we work with. To support additionals, you
 * could create another macro like:
 @code
 #define MALLOC_NET(size)	tracked_alloc(g_net_memctx, ...
 ...
 @endcode
 * and provide your context as another extern.
 *
 * Will not exist if USING_MEMORY_DEBUGGING is not defined.
 *
 * Most interaction with this class should be done with the special macros:
 * - MALLOC
 * - REALLOC
 * - FREE
 * These handle providing the line number, file, and function, and will define
 * to the real malloc/realloc/free automatically if not debugging memory.
 *
 * Naturally, anything allocated/freed by malloc/realloc/free outside of our
 * macros will not be tracked.
 * 
 * You must initialize the context with mem_context_init(), and cleanup with
 * mem_context_destroy().
 *
 * @struct mem_context
 */
struct mem_context
{
	uint32_t	allocs;			/**< The amount of times new has been called successfully */
	uint32_t	frees;			/**< The amount of times delete has been called successfully */
	uint32_t	current_allocated;	/**< Currently allocated amount of bytes */
	uint32_t	total_allocated;	/**< The total amount of allocated bytes */

#if defined(_WIN32)
	CRITICAL_SECTION	cs;
#else
	pthread_mutex_t		lock;
	pthread_mutexattr_t	lock_attrib;
#endif

	/** A list of all the memblock_header objects created and active */
	TAILQ_HEAD(st_headname, memblock_header)	memblocks;
};



/**
 * Checks a block of memory, ensuring both the header and footer are not
 * corrupt, and the rest of the block matches the original requestors
 * specifications.
 *
 * Define DISABLE_MEMORY_CHECK_TO_STDOUT to prevent stage-by-stage
 * output of the checking process.
 *
 * @param[in] memory_block The block of memory to check
 * @retval E_MEMORY_ERROR The relevant memory error code as to the block
 * status; should hopefully always be EC_NoError
 */
enum E_MEMORY_ERROR
check_block(
	struct memblock_header* memory_block
);


/**
 * Destroys a previously initialized memory context. If any memory blocks exist
 * within the list, it is declared as a memory leak, and will call the
 * output_memory_info() function to report it.
 *
 * @param[in] context The memory context to destroy
 */
void
mem_context_destroy(
	struct mem_context* const context
);


/**
 * Initializes the memory context, ready for usage.
 * 
 * Must be destroyed via mem_context_destroy().
 *
 * @param[in] context The memory context to initialize
 */
void
mem_context_init(
	struct mem_context* const context
);


/**
 * Called only in the destructor, but available for calling manually if
 * desired; will always output the memory stats for the application run,
 * but will also write out the information on any unfreed memory.
 *
 * Outputs to MEM_LEAK_LOG_NAME, but if it's not writable, it is printed
 * to stderr instead.
 *
 * @param[in] context The memory context to work with
 */
void
output_memory_info(
	struct mem_context* const context
);


/**
 * Tracked version of malloc - use the MALLOC macro to call this, as it
 * will setup the parameters for you, barring the num_bytes.
 *
 * This function will dynamically alter the requested amount in order to
 * leave space for a header and footer; the client code does not need to
 * handle, or be aware, of this fact.
 *
 * @param[in] context The memory context to work with
 * @param[in] num_bytes The number of bytes to allocate
 * @param[in] file The file this method was called in
 * @param[in] function The function this method was called in
 * @param[in] line The line number in the file this method was called in
 * @return A pointer to the allocated memory, or a nullptr if the
 * allocation failed.
 */
void*
tracked_alloc(
	struct mem_context* const context,
	const uint32_t num_bytes,
	const char* file,
	const char* function,
	const uint32_t line
);


/**
 * Tracked version of free - use the FREE macro to call this, for
 * consistency and potential future changes.
 *
 * In compliance with the C standard, if memory is a nullptr, this
 * function performs no action. No action is also performed if a
 * memory validation check fails (i.e. the supplied block is corrupt or
 * otherwise invalid), as doing so could crash the application, and
 * miss logging the information.
 *
 * @param[in] context The memory context to work with
 * @param[in] memory A pointer to the memory previously allocated by
 * tracked_alloc (which should be called via MALLOC)
 */
void
tracked_free(
	struct mem_context* const context,
	void* memory
);


/**
 * Tracked version of realloc - use the REALLOC macro to call this.
 *
 * In compliance with the C standard, if memory_block is a nullptr,
 * the end result is the same as calling tracked_alloc (i.e. malloc). 
 * The same applies if new_num_bytes is 0 - tracked_free (i.e. free) 
 * will be called on the memory block.
 *
 * No validation is performed on the original block of memory, and
 * unlike the real realloc, we call TrackedAlloc regardless of size
 * differences and other parameters. The previous memory is then moved
 * into this newly allocated block, and the original freed.
 *
 * As a result, a TrackedRealloc guarantees that the returned pointer
 * will never be the same as the one passed in.
 *
 * @param[in] context The memory context to work with
 * @param[in] memory The pointer to memory returned by TrackedAlloc()
 * @param[in] new_num_bytes The new number of bytes to allocate
 * @param[in] file The file this method was called in
 * @param[in] function The function this method was called in
 * @param[in] line The line number in the file this method was called in
 * @retval nullptr if the function fails due to invalid parameters, or
 * the call to realloc fails
 * @return A pointer to the usable block of memory allocated
 */
void*
tracked_realloc(
	struct mem_context* const context,
	void* memory,
	const uint32_t new_num_bytes,
	const char* file,
	const char* function,
	const uint32_t line
);


/**
 * Validates a tracked bit of memory; if no pointer is supplied, so 
 * nullptr is passed in, every tracked block currently present in the
 * class is validated.
 *
 * If present, the memory passed in must be at the pointer to the memory
 * returned by the tracked_alloc/tracked_realloc caller.
 *
 * Internally calls check_block().
 *
 * @param[in] context The mem_context memory resides in
 * @param[in] memory A pointer to the memory to validate
 * @retval true if the memory is not corrupt and usable
 * @retval false if the memory failed one of the checks
 */
bool
validate_memory(
	struct mem_context* const context,
	void* memory
);




/** Macro to create tracked memory */
	#define MALLOC(size)		tracked_alloc(&g_mem_ctx, size, __FILE__, __FUNCTION__, __LINE__)

/** Macro to reallocate tracked memory */
	#define REALLOC(ptr, size)	tracked_realloc(&g_mem_ctx, ptr, size, __FILE__, __FUNCTION__, __LINE__)

/** Macro to delete tracked memory */
	#define FREE(varname)		tracked_free(&g_mem_ctx, varname)

/** The amount of bytes (limit) printed to output under the Data field */
	#define MEM_OUTPUT_LIMIT	1024

/** The default, global, memory context to use for all memory operations */
extern struct mem_context		g_mem_ctx;

#else
	/* if we're here, we don't want to debug the memory, so just set the
	 * macros to call the original, non-hooked functions. */

#	define MALLOC(size)		malloc(size)
#	define REALLOC(ptr, size)	realloc(ptr, size)
#	define FREE(varname)		free(varname)

#endif	// USING_MEMORY_DEBUGGING



#endif	// TRACKED_MEMORY_H_INCLUDED
