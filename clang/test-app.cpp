#include <memory>

class Thing {
public:
  Thing() = default;
  Thing( int value ) : value_( value ) {}
private:
  int value_;
};

class Item {
};

int main() {
  Item i0;
  Thing t0;
  Thing t1( t0 );
  Thing* t2 = new Thing;
  std::shared_ptr< Thing > t3( new Thing );
  Thing t4( 42 );
  delete t2;
  return 0;
}
