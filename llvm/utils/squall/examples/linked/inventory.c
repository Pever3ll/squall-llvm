#include "inventory.h"

struct item inventory[] = {
    {"apple", 12, 30}, {"pear", 3, 45}, {"fig", 40, 12}, {"lime", 0, 25}};
const int inventory_size = sizeof inventory / sizeof inventory[0];

// Each translation unit has its own `counter`; linking must keep them apart.
static int counter;

int restock(int amount) {
  for (int i = 0; i < inventory_size; ++i)
    if (inventory[i].count < 10) {
      inventory[i].count = (short)(inventory[i].count + amount);
      ++counter;
    }
  return counter;
}

int total_value(void) {
  int total = 0;
  for (int i = 0; i < inventory_size; ++i)
    total += inventory[i].count * inventory[i].price;
  return total;
}
