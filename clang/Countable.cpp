#include <stdio.h>
#include <typeinfo>

#include "Countable.hpp"

Counter* gCounter = new Counter;

Counter::Counter() {
}

Counter::~Counter() {
}

void Counter::onCreated( const void* ptr, const std::type_info& type ) {
  fprintf( stderr, "a %s = %p\n", type.name(), ptr );
}

void Counter::onDeleted( const void* ptr ) {
  fprintf( stderr, "d %p\n", ptr );
}
