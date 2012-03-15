#include <memory>

class Thing {
};

int main() {
  Thing thing;
  Thing* pThing = new Thing;
  std::shared_ptr< Thing > spThing( new Thing );
  delete pThing;
  return 0;
}
