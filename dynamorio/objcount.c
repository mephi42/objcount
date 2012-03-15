#include <demangle.h>
#include <dr_api.h>
#include <dr_events.h>
#include <../ext/drcontainers/hashtable.h>
#include <../ext/drsyms/drsyms.h>
#include <../ext/drwrap/drwrap.h>
#include <string.h>

static file_t outFile;
static void* outMutex = NULL;

static hashtable_t wraps;
#define FLAG_BASE_OBJECT_DESTRUCTOR      ( 1 << 0 )
#define FLAG_COMPLETE_OBJECT_DESTRUCTOR  ( 1 << 1 )
#define FLAG_DEALLOCATING_DESTRUCTOR     ( 1 << 2 )
#define FLAG_BASE_OBJECT_CONSTRUCTOR     ( 1 << 3 )
#define FLAG_COMPLETE_OBJECT_CONSTRUCTOR ( 1 << 4 )
#define FLAG_ALLOCATING_CONSTRUCTOR      ( 1 << 5 )
struct wrap {
  app_pc address;
  unsigned int flags;
};
static struct wrap* alloc_wrap() {
  struct wrap* wrap = ( struct wrap* )dr_global_alloc( sizeof( struct wrap ) );
  if( wrap == NULL ) {
    dr_printf( "Failed to allocate memory for struct wrap\n" );
    exit( 1 );
  }
  return wrap;
}
static void free_wrap( void* wrap ) {
  dr_global_free( wrap, sizeof( struct wrap ) );
}

static void wrapper( void* ctx, void** data ) {
  dr_mutex_lock( outMutex );
  dr_fprintf( outFile, "x %p\n", (void*)drwrap_get_func( ctx ) );
  dr_mutex_unlock( outMutex );
}

static void wrapSymbol( app_pc pc,
                        struct demangle_component* qual,
                        struct demangle_component* info ) {
  size_t size;
  char* className = cplus_demangle_print( DMGL_NO_OPTS, qual, 64, &size );

  hashtable_lock( &wraps );
  struct wrap* wrap = ( struct wrap* )hashtable_lookup( &wraps, (void*)pc );
  if( wrap == NULL ) {
    // Not wrapped yet.
    if( !drwrap_wrap( pc, &wrapper, NULL ) ) {
      dr_printf( "drwrap_wrap(%s %s of kind %i = %p) failed\n",
                 className,
                 info->type == DEMANGLE_COMPONENT_CTOR ? "ctor" : "dtor",
                 info->type == DEMANGLE_COMPONENT_CTOR ? info->u.s_ctor.kind :
                                                         info->u.s_dtor.kind,
                 pc );
      exit( 1 );
    }

    wrap = alloc_wrap();
    memset( wrap, 0, sizeof( struct wrap ) );
    wrap->address = pc;
    hashtable_add( &wraps, (void*)pc, wrap );
  } else {
    // Already wrapped - do nothing.
  }
  char* eventType = "?";
  if( info->type == DEMANGLE_COMPONENT_CTOR ) {
    switch( info->u.s_ctor.kind ) {
    case gnu_v3_complete_object_allocating_ctor:
      wrap->flags |= FLAG_ALLOCATING_CONSTRUCTOR;
      eventType = "A";
      break;
    case gnu_v3_complete_object_ctor:
      wrap->flags |= FLAG_COMPLETE_OBJECT_CONSTRUCTOR;
      eventType = "C";
      break;
    case gnu_v3_base_object_ctor:
      wrap->flags |= FLAG_BASE_OBJECT_CONSTRUCTOR;
      eventType = "B";
      break;
    }
  } else {
    switch( info->u.s_dtor.kind ) {
    case gnu_v3_deleting_dtor:
      wrap->flags |= FLAG_DEALLOCATING_DESTRUCTOR;
      eventType ="a";
      break;
    case gnu_v3_complete_object_dtor:
      wrap->flags |= FLAG_COMPLETE_OBJECT_DESTRUCTOR;
      eventType = "c";
      break;
    case gnu_v3_base_object_dtor:
      wrap->flags |= FLAG_BASE_OBJECT_DESTRUCTOR;
      eventType = "b";
      break;
    }
  }
  hashtable_unlock( &wraps );

  dr_mutex_lock( outMutex );
  dr_fprintf( outFile, "%s %p %s\n", eventType, (void*)pc, className );
  dr_mutex_unlock( outMutex );

  free( className );
}

static bool onSymbol( const char* sym, size_t offset, void* data ) {
  app_pc pc = ((module_data_t*)data)->start + offset;

  if( sym[0] != '_' || sym[1] != 'Z' ) {
    // Not a mangled name - ignore.
    return true;
  }

  void* toFree = NULL;
  struct demangle_component* info = cplus_demangle_v3_components( sym, DMGL_NO_OPTS, &toFree );
  if( info == NULL ) {
    // Could not demangle - no big deal.
    return true;
  }

  struct demangle_component* qual = NULL;
  if( info->type == DEMANGLE_COMPONENT_QUAL_NAME ) {
    qual = info->u.s_binary.left;
    info = info->u.s_binary.right;
  }

  if( info->type == DEMANGLE_COMPONENT_CTOR ||
      info->type == DEMANGLE_COMPONENT_DTOR ) {
    wrapSymbol( pc, qual, info );
  }

  free( toFree );
  return true;
}

static void onLoad( void* ctx, const module_data_t* m, bool loaded ) {
  dr_printf( "In onLoad(%s)\n", m->full_path );
  drsym_error_t rc = drsym_enumerate_symbols( m->full_path, onSymbol, (void*)m, 0 );
  if( DRSYM_SUCCESS != rc ) {
    dr_printf( "  drsym_enumerate_symbols() failed: %i\n", rc );
  }
}

static void onExit() {
  dr_printf( "In onExit()\n" );

  // Clean up hashtable.
  hashtable_delete( &wraps );

  // Clean up output.
  dr_mutex_destroy( outMutex );
  dr_close_file( outFile );

  // Clean up extensions.
  drwrap_exit();
  drsym_exit();
}

DR_EXPORT void dr_init( client_id_t id ) {
  dr_printf( "In dr_init()\n" );

  // Initialize extensions.
  drsym_error_t rc = drsym_init( 0 );
  if( DRSYM_SUCCESS != rc ) {
    dr_printf( "drsym_init() failed: %i\n", rc );
    exit( 1 );
  }

  bool wrapInit = drwrap_init();
  if( !wrapInit ) {
    dr_printf( "drwrap_init() failed\n" );
    exit( 1 );
  }

  // Set up output.
  outFile = dr_open_file( "objcount.out", DR_FILE_WRITE_OVERWRITE );
  outMutex = dr_mutex_create();

  // Set up hashtable.
  hashtable_init_ex( &wraps,      // table
                     16,          // num_bits
                     HASH_INTPTR, // hashtype
                     false,       // str_dup
                     false,       // synch
                     &free_wrap,  // free_payload_func
                     NULL,        // hash_key_func
                     NULL );      // cmp_key_func

  // Register for events.
  dr_register_module_load_event( onLoad );
  dr_register_exit_event( onExit );
}
