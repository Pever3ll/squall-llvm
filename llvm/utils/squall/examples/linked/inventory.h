// A program of three translation units, linked into one Squall image.

#ifndef SQUALL_EXAMPLE_INVENTORY_H
#define SQUALL_EXAMPLE_INVENTORY_H

struct item {
  char name[8];
  short count;
  int price;
};

typedef int (*score_fn)(const struct item *);

extern struct item inventory[];
extern const int inventory_size;
extern score_fn scorers[];

int restock(int amount);
int total_value(void);
int best_by(int scorer);
int checksum(const char *s);

#endif
