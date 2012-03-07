#ifndef _COUNTABLE
#define _COUNTABLE

namespace std {
  class type_info;
}

class Counter {
public:
  Counter();
  ~Counter();
  void onCreated( const void* ptr, const std::type_info& type );
  void onDeleted( const void* ptr );
};

extern Counter* gCounter;

template< typename T >
class Countable {
public:
  Countable();
  ~Countable();
};

template< typename T >
Countable< T >::Countable() {
  if( gCounter ) {
    gCounter->onCreated( this, typeid( T ) );
  }
}

template< typename T >
Countable< T >::~Countable() {
  if( gCounter ) {
    gCounter->onDeleted( this );
  }
}

#endif
