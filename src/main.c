
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "tracked_memory.h"



int32_t
main(
	int32_t argc,
	char** argv
)
{
	uint32_t	leak_alloc_1 = 256;
	uint32_t	leak_alloc_2 = 128;
	uint32_t	free_alloc = 24;
	char*		leaked_buffer_1;
	char*		leaked_buffer_2;
	char*		freed_buffer;

	// must initialize context before use
	mem_context_init(&g_mem_ctx);

	leaked_buffer_1 = (char*)MALLOC(leak_alloc_1);
	leaked_buffer_2 = (char*)MALLOC(leak_alloc_2);
	freed_buffer = (char*)MALLOC(free_alloc);

	printf("\n############################\n");
	printf("### C Memory Manager PoC ###\n");
	printf("############################\n\n");
	printf("allocated %i bytes of memory\n", leak_alloc_1);
	printf("allocated %i bytes of memory\n", leak_alloc_2);
	printf("allocated %i bytes of memory\n", free_alloc);

	strncpy(leaked_buffer_1, "This is allocated memory that will not be freed", leak_alloc_1 - 1);
	strncpy(freed_buffer, "This buffer is freed", free_alloc-1);
	strncpy(leaked_buffer_2, argv[0], leak_alloc_2 - 1);

	printf("freeing one buffer...\n");
	FREE(freed_buffer);
	printf("closing the application without freeing the remaining allocated memory...\n\n");

	// destroy context when finished, so it can output meminfo
	mem_context_destroy(&g_mem_ctx);
	
	return EXIT_SUCCESS;
}
