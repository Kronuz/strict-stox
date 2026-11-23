// Differential test of stod and stof against libc strtod and strtof on decimal text, with and without white space or a '+' before
// it, with and without text after it, over the fuzz corpus and N random texts (default 1,000,000; argv[1]). libc runs in a child
// process over a batch of texts, so that a text on which it aborts (Apple's strtod) is found by halving and left out.
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "oracle.hh"

namespace {

struct Libc {
	oracle::Result d, f;  // strtod and strtof, whole text
	oracle::Result di, fi;  // with an idx
	bool ok;
};

void run_child(const std::vector<std::string>& v, int fd) {
	for (const std::string& s : v) {
		const oracle::Result r[4] = {oracle::libc_result<double>(s, false), oracle::libc_result<float>(s, false), oracle::libc_result<double>(s, true), oracle::libc_result<float>(s, true)};
		if (write(fd, r, sizeof(r)) != static_cast<ssize_t>(sizeof(r))) { _exit(2); }
	}
}

void libc_batch(const std::vector<std::string>& v, std::size_t lo, std::size_t hi, std::vector<Libc>& out, std::size_t& aborted) {
	if (lo == hi) { return; }
	int p[2];
	if (pipe(p) != 0) { std::abort(); }
	const std::vector<std::string> part(v.begin() + static_cast<std::ptrdiff_t>(lo), v.begin() + static_cast<std::ptrdiff_t>(hi));
	const pid_t pid = fork();
	if (pid == 0) {
		close(p[0]);
		run_child(part, p[1]);
		_exit(0);
	}
	close(p[1]);
	std::vector<Libc> got;
	oracle::Result r[4];
	while (true) {
		std::size_t n = 0;
		while (n < sizeof(r)) {
			const ssize_t k = read(p[0], reinterpret_cast<char*>(r) + n, sizeof(r) - n);
			if (k <= 0) { break; }
			n += static_cast<std::size_t>(k);
		}
		if (n < sizeof(r)) { break; }
		got.push_back(Libc{r[0], r[1], r[2], r[3], true});
	}
	close(p[0]);
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && got.size() == hi - lo) {
		for (std::size_t i = 0; i < got.size(); ++i) { out[lo + i] = got[i]; }
		return;
	}
	if (hi - lo == 1) {
		++aborted;
		return;
	}
	const std::size_t mid = lo + (hi - lo) / 2;
	libc_batch(v, lo, mid, out, aborted);
	libc_batch(v, mid, hi, out, aborted);
}

std::string random_text(std::mt19937_64& g) {
	static const char* const leads[] = {"", "", "", " ", "+", " +", "\t", "  \n+", "\v\f\r ", "-", " -"};
	auto n = [&](unsigned k) { return static_cast<unsigned>(g() % k); };
	std::string t = leads[n(11)];
	const unsigned ni = n(4) == 0 ? n(60) : n(20), nf = n(4) == 0 ? n(60) : n(20);
	for (unsigned i = 0; i < ni; ++i) { t += static_cast<char>('0' + (n(3) == 0 ? 0 : n(10))); }
	if (n(3) != 0 || (ni == 0 && nf == 0)) {
		t += '.';
		for (unsigned i = 0; i < nf; ++i) { t += static_cast<char>('0' + (n(3) == 0 ? 0 : n(10))); }
	}
	if (n(3) == 0) {
		t += n(2) ? 'e' : 'E';
		if (n(2)) { t += n(2) ? '-' : '+'; }
		t += std::to_string(n(4) == 0 ? n(1000) : n(40));
	}
	if (n(6) == 0) {
		static const char* const tails[] = {"x", " ", "f", ".", "e", "e+", "..1", " 5", "\n", "-"};
		t += tails[n(10)];
	}
	return t;
}

}  // namespace

int main(int argc, char** argv) {
	const std::size_t N = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
	std::vector<std::string> texts;
	for (const char* c : {"fuzz/corpus/seed-0", "fuzz/corpus/seed-1", "fuzz/corpus/seed-2", "fuzz/regressions/apple-strtofp-minimal.txt", "fuzz/regressions/apple-strtofp-original.txt"}) {
		std::ifstream f(c, std::ios::binary);
		if (!f) { continue; }
		const std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		for (const char* lead : {"", " ", "+", " +", "\t "}) {
			texts.push_back(lead + body);
			for (std::size_t cut : {std::size_t(1), body.size() / 3, body.size() / 2, body.size() - 1}) {
				if (cut != 0 && cut < body.size()) { texts.push_back(lead + body.substr(0, cut)); }
			}
		}
	}
	std::mt19937_64 g(20261123);
	while (texts.size() < N) { texts.push_back(random_text(g)); }

	long failures = 0;
	std::size_t aborted = 0, compared = 0;
	const std::size_t batch = 2000;
	for (std::size_t lo = 0; lo < texts.size(); lo += batch) {
		const std::size_t hi = std::min(texts.size(), lo + batch);
		std::vector<std::string> chunk(texts.begin() + static_cast<std::ptrdiff_t>(lo), texts.begin() + static_cast<std::ptrdiff_t>(hi));
		std::vector<Libc> res(chunk.size(), Libc{{}, {}, {}, {}, false});
		libc_batch(chunk, 0, chunk.size(), res, aborted);
		for (std::size_t i = 0; i < chunk.size(); ++i) {
			if (!res[i].ok) { continue; }
			const std::string& s = chunk[i];
			++compared;
			const oracle::Result a[4] = {oracle::api_result<double>(s, false), oracle::api_result<float>(s, false), oracle::api_result<double>(s, true), oracle::api_result<float>(s, true)};
			const oracle::Result* b[4] = {&res[i].d, &res[i].f, &res[i].di, &res[i].fi};
			for (int k = 0; k < 4; ++k) {
				if (!oracle::same(a[k], *b[k], k >= 2)) {
					if (++failures <= 20) { std::fprintf(stderr, "differs (%s%s, %zu bytes): bits %llx errno %d idx %zu, libc bits %llx errno %d idx %zu\n", k % 2 ? "stof" : "stod", k >= 2 ? " idx" : "", s.size(), (unsigned long long)a[k].bits, a[k].err, a[k].idx, (unsigned long long)b[k]->bits, b[k]->err, b[k]->idx); }
				}
			}
		}
	}
	std::printf("%zu texts compared (4 entry forms each), %zu left out (libc aborts), %ld differences\n", compared, aborted, failures);
	return failures == 0 ? 0 : 1;
}
