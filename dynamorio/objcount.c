#include <demangle.h>
#include <dr_api.h>
#include <dr_events.h>
#include <../ext/drcontainers/hashtable.h>
#include <../ext/drsyms/drsyms.h>
#include <../ext/drwrap/drwrap.h>
#include <string.h>

/** Output file. */
static file_t outFile;
static void* outMutex = NULL;

/** Active wrapper contexts. */
static hashtable_t wraps;

/** Number of milliseconds between timestamps. */
static const uint64 timestampIntervalMillis = 100;

/** Time to record next timestamp. */
static uint64 nextTimestampMillis = 0;

#define FLAG_BASE_OBJECT_DESTRUCTOR      ( 1 << 0 )
#define FLAG_COMPLETE_OBJECT_DESTRUCTOR  ( 1 << 1 )
#define FLAG_DEALLOCATING_DESTRUCTOR     ( 1 << 2 )
#define FLAG_BASE_OBJECT_CONSTRUCTOR     ( 1 << 3 )
#define FLAG_COMPLETE_OBJECT_CONSTRUCTOR ( 1 << 4 )
#define FLAG_ALLOCATING_CONSTRUCTOR      ( 1 << 5 )

static void wrapperPre( void* ctx, void** data );

/** Wrapper context. */
struct wrap {
  app_pc address;
  unsigned int flags;
};

/** Allocates storage for wrapper context. */
static struct wrap* alloc_wrap() {
  struct wrap* wrap = ( struct wrap* )dr_global_alloc( sizeof( struct wrap ) );
  if( wrap == NULL ) {
    dr_printf( "Failed to allocate memory for struct wrap\n" );
    exit( 1 );
  }
  return wrap;
}

/** Deletes wrapper context and removes corresponding wrapper. */
static void free_wrap( void* rawWrap ) {
  struct wrap* wrap = ( struct wrap* )rawWrap;
  if( !drwrap_unwrap( wrap->address, &wrapperPre, NULL ) ) {
    dr_printf( "drwrap_unwrap(%p) failed\n", (void*)wrap->address );
    exit( 1 );
  }
  dr_global_free( wrap, sizeof( struct wrap ) );
}

/** Wrapper pre-callback. */
static void wrapperPre( void* ctx, void** data ) {
  dr_mutex_lock( outMutex );

  uint64 currentTimeMillis = dr_get_milliseconds();
  if( currentTimeMillis > nextTimestampMillis ) {
    dr_fprintf( outFile, "T 0x%.16llx\n", currentTimeMillis );
    nextTimestampMillis = currentTimeMillis + timestampIntervalMillis;
  }

  dr_fprintf( outFile, "X %p %p\n", (void*)drwrap_get_func( ctx ),
                                    drwrap_get_arg( ctx, 0 ) );
  dr_mutex_unlock( outMutex );
}

/** Installs wrapper for a given symbol. */
static void wrapSymbol( const char* sym,
                        module_data_t* module,
                        size_t offset,
                        struct demangle_component* qual,
                        struct demangle_component* info ) {
  app_pc pc = module->start + offset;
  if( offset == 0 || pc == 0 ) {
    dr_printf( "Skipping symbol %s\n", sym );
    return;
  }

  size_t size;
  char* className = cplus_demangle_print( DMGL_NO_OPTS, qual, 64, &size );

  hashtable_lock( &wraps );
  struct wrap* wrap = ( struct wrap* )hashtable_lookup( &wraps, (void*)pc );
  if( wrap == NULL ) {
    // Not wrapped yet.
    if( !drwrap_wrap( pc, &wrapperPre, NULL ) ) {
      dr_printf( "drwrap_wrap(%s %s of kind %i = +%p = %p) failed\n",
                 className,
                 info->type == DEMANGLE_COMPONENT_CTOR ? "ctor" : "dtor",
                 info->type == DEMANGLE_COMPONENT_CTOR ? info->u.s_ctor.kind :
                                                         info->u.s_dtor.kind,
                 (void*)offset,
                 (void*)pc );
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
    wrapSymbol( sym, (module_data_t*)data, offset, qual, info );
  }

  free( toFree );
  return true;
}

static void onLoad( void* ctx, const module_data_t* m, bool loaded ) {
  dr_printf( "In onLoad(%s, %p-%p)\n", m->full_path, m->start, m->end );

  dr_mutex_lock( outMutex );
  dr_fprintf( outFile, "M %p %p %s\n", m->start, m->end, m->full_path );
  dr_mutex_unlock( outMutex );

  drsym_error_t rc = drsym_enumerate_symbols( m->full_path, onSymbol, (void*)m, 0 );
  if( DRSYM_SUCCESS != rc ) {
    dr_printf( "  drsym_enumerate_symbols() failed: %i\n", rc );
  }
}

static void onUnload( void* ctx, const module_data_t* m ) {
  dr_printf( "In onUnload(%s, %p-%p)\n", m->full_path, m->start, m->end );

  dr_mutex_lock( outMutex );
  dr_fprintf( outFile, "m %p %p %s\n", (void*)m->start,
                                       (void*)m->end,
                                       m->full_path );
  dr_mutex_unlock( outMutex );

  hashtable_lock( &wraps );
  hashtable_remove_range( &wraps, (void*)m->start, (void*)m->end );
  hashtable_unlock( &wraps );
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
  char fileName[256];
  unsigned int pid = (unsigned int)dr_get_process_id();
  dr_snprintf( fileName, sizeof( fileName ), "objcount-%u.out", pid );
  fileName[sizeof( fileName ) - 1] = 0;
  outFile = dr_open_file( fileName, DR_FILE_WRITE_OVERWRITE );
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
