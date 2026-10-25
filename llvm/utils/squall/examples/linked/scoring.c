#include "inventory.h"

static int counter = 100;

static int by_value(const struct item *it) { return it->count * it->price; }
static int by_name(const struct item *it) { return checksum(it->name); }
static int by_scarcity(const struct item *it) { return -it->count; }

score_fn scorers[] = {by_value, by_name, by_scarcity};

int checksum(const char *s) {
  unsigned h = 7;
  while (*s)
    h = h * 31u + (unsigned char)*s++;
  ++counter;
  return (int)(h & 0xffff);
}

int best_by(int scorer) {
  score_fn f = scorers[scorer % 3];
  int best = 0;
  for (int i = 1; i < inventory_size; ++i)
    if (f(&inventory[i]) > f(&inventory[best]))
      best = i;
  return best * 1000 + counter;
}
