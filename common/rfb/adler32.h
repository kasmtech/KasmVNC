#include <stdint.h>

// contributed by Dmitry Artamonov
// this is *deterministic*
class Adler32 {
	uint32_t sum1, sum2;
	int len;

	enum {
		Base = 65521
	};

public:
	uint32_t hash;

	Adler32(int window) : sum1(1), sum2(0), len(window), hash(0) {}

	void eat(const uint8_t inchar) {
		sum1 = (sum1 + inchar) % Base;
		sum2 = (sum2 + sum1) % Base;

		hash = (sum2 << 16) | sum1;
	}

	void reset() {
	  sum1 = 1;
	  sum2 = 0;
	  hash = 0;
	}

	void update(const uint8_t outchar, const uint8_t inchar) {
		int sum2 = (hash >> 16) & 0xffff;
		int sum1 = hash & 0xffff;

		sum1 += inchar - outchar;
		if (sum1 >= Base)
		{
			sum1 -= Base;
		}
		else if (sum1 < 0)
		{
			sum1 += Base;
		}

		sum2 = ((int)(sum2 - len * outchar + sum1 - 1) % (int)Base);
		if (sum2 < 0)
		{
			sum2 += Base;
		}
		hash = (sum2 << 16) | sum1;
	}
};
