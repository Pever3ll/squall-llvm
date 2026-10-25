#include "inventory.h"

int run(int amount, int scorer) {
  int restocked = restock(amount);
  return total_value() * 10 + restocked + best_by(scorer) * 100000 +
         checksum(inventory[scorer & 3].name);
}
