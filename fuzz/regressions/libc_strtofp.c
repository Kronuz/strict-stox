#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
	if (argc != 3) { return 2; }
	FILE* input = fopen(argv[2], "rb");
	if (!input) { return 2; }
	char text[4096];
	size_t size = fread(text, 1, sizeof(text) - 1, input);
	int invalid = ferror(input) || !feof(input);
	fclose(input);
	if (invalid) { return 2; }
	text[size] = '\0';
	char* end = text;
	long double value;
	errno = 0;
	if (!strcmp(argv[1], "strtof")) { value = strtof(text, &end); }
	else if (!strcmp(argv[1], "strtod")) { value = strtod(text, &end); }
	else if (!strcmp(argv[1], "strtold")) { value = strtold(text, &end); }
	else { return 2; }
	printf("%s: size=%zu consumed=%zu errno=%d value=%La\n",
		argv[1], size, (size_t)(end - text), errno, value);
	return 0;
}
