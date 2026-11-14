#include "strict_stox.hh"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

int main(int argc, char** argv) {
	if (argc != 3 && argc != 4) { return 2; }
	FILE* input = std::fopen(argv[2], "rb");
	if (!input) { return 2; }
	char text[4096];
	const std::size_t size = std::fread(text, 1, sizeof(text) - 1, input);
	const bool invalid = std::ferror(input) || !std::feof(input);
	std::fclose(input);
	if (invalid) { return 2; }
	const std::string_view view(text, size);
	std::size_t used = 0;
	if (argc == 4 && std::strcmp(argv[3], "prefix")) { return 2; }
	std::size_t* index = argc == 4 ? &used : nullptr;
	int error = 0;
	long double value;
	errno = EDOM;
	if (!std::strcmp(argv[1], "stof")) { value = strict_stof(&error, view, index); }
	else if (!std::strcmp(argv[1], "stod")) { value = strict_stod(&error, view, index); }
	else if (!std::strcmp(argv[1], "stold")) { value = strict_stold(&error, view, index); }
	else { return 2; }
	std::printf("%s: size=%zu consumed=%zu error=%d errno=%d value=%La\n",
		argv[1], size, used, error, errno, value);
	return 0;
}
