#include "frida-gadget.h"

#include "frida-interfaces.h"

#ifndef G_OS_WIN32
# include <dlfcn.h>
#endif
#ifdef HAVE_ANDROID
# include <android/log.h>
# include <unistd.h>
#else
# include <stdio.h>
# ifdef HAVE_DARWIN
#  include <CoreFoundation/CoreFoundation.h>
#  include <dlfcn.h>

typedef struct _FridaCFApi FridaCFApi;
typedef gint32 CFLogLevel;

enum _CFLogLevel
{
  kCFLogLevelEmergency = 0,
  kCFLogLevelAlert     = 1,
  kCFLogLevelCritical  = 2,
  kCFLogLevelError     = 3,
  kCFLogLevelWarning   = 4,
  kCFLogLevelNotice    = 5,
  kCFLogLevelInfo      = 6,
  kCFLogLevelDebug     = 7
};

struct _FridaCFApi
{
  const CFAllocatorRef * kCFAllocatorDefault;
  const CFStringRef * kCFRunLoopCommonModes;

  void (* CFRelease) (CFTypeRef cf);

  CFStringRef (* CFStringCreateWithCString) (CFAllocatorRef alloc, const char * c_str, CFStringEncoding encoding);

  CFRunLoopRef (* CFRunLoopGetMain) (void);
  void (* CFRunLoopRun) (void);
  void (* CFRunLoopStop) (CFRunLoopRef loop);
  CFRunLoopTimerRef (* CFRunLoopTimerCreate) (CFAllocatorRef allocator, CFAbsoluteTime fire_date, CFTimeInterval interval, CFOptionFlags flags, CFIndex order, CFRunLoopTimerCallBack callout, CFRunLoopTimerContext * context);
  void (* CFRunLoopAddTimer) (CFRunLoopRef loop, CFRunLoopTimerRef timer, CFStringRef mode);
  void (* CFRunLoopTimerInvalidate) (CFRunLoopTimerRef timer);

  void (* CFLog) (CFLogLevel level, CFStringRef format, ...);
};

static void * frida_gadget_wait_for_permission_to_resume_then_stop_loop (void * user_data);
static void on_keep_alive_timer_fire (CFRunLoopTimerRef timer, void * info);

static FridaCFApi * frida_cf_api_try_get (void);

# endif
#endif

typedef struct _FridaThreadCreateContext FridaThreadCreateContext;

#ifdef G_OS_WIN32
typedef unsigned NativeThreadFuncReturnType;
# define NATIVE_THREAD_FUNC_API __stdcall
#else
typedef void * NativeThreadFuncReturnType;
# define NATIVE_THREAD_FUNC_API
#endif
typedef NativeThreadFuncReturnType (NATIVE_THREAD_FUNC_API * NativeThreadFunc) (void * data);

struct _FridaThreadCreateContext
{
  NativeThreadFunc thread_func;
  void * thread_data;

  FridaGadgetAutoIgnorer * ignorer;
};

#ifndef G_OS_WIN32

typedef struct _FridaTlsKeyContext FridaTlsKeyContext;

struct _FridaTlsKeyContext
{
  void (* destructor) (void *);
  gboolean replaced;

  FridaGadgetAutoIgnorer * ignorer;
};

static void frida_tls_key_context_free (FridaTlsKeyContext * ctx);

#endif

static gpointer run_main_loop (gpointer data);
static gboolean stop_main_loop (gpointer data);

static void frida_gadget_on_assert_failure (const gchar * log_domain, const gchar * file, gint line, const gchar * func, const gchar * message, gpointer user_data) G_GNUC_NORETURN;
static void frida_gadget_on_log_message (const gchar * log_domain, GLogLevelFlags log_level, const gchar * message, gpointer user_data);

static void frida_gadget_auto_ignorer_shutdown (FridaGadgetAutoIgnorer * self);

static gpointer frida_get_address_of_thread_create_func (void);
static NativeThreadFuncReturnType NATIVE_THREAD_FUNC_API frida_thread_create_proxy (void * data);
static void frida_thread_create_context_free (FridaThreadCreateContext * ctx);

static void * frida_linker_stub_warmup_thread (void * data);

static GThread * main_thread;
static GMainLoop * main_loop;
static GMainContext * main_context;

static GPrivate frida_thread_create_context_key = G_PRIVATE_INIT ((GDestroyNotify) frida_thread_create_context_free);

__attribute__ ((constructor)) static void
on_load (void)
{
  frida_gadget_load ();

#ifdef HAVE_DARWIN
  FridaCFApi * api = frida_cf_api_try_get ();
  if (api != NULL)
  {
    CFRunLoopRef loop;
    CFAbsoluteTime distant_future;
    CFRunLoopTimerRef timer;
    pthread_t thread;

    loop = api->CFRunLoopGetMain ();

    distant_future = DBL_MAX;
    timer = api->CFRunLoopTimerCreate (*(api->kCFAllocatorDefault), distant_future, 0, 0, 0, on_keep_alive_timer_fire, NULL);
    api->CFRunLoopAddTimer (loop, timer, *(api->kCFRunLoopCommonModes));

    pthread_create (&thread, NULL, frida_gadget_wait_for_permission_to_resume_then_stop_loop, loop);
    pthread_detach (thread);

    api->CFRunLoopRun ();

    api->CFRunLoopTimerInvalidate (timer);
    api->CFRelease (timer);
  }
  else
#endif
  {
    frida_gadget_wait_for_permission_to_resume ();
  }

  (void) frida_linker_stub_warmup_thread;
}

__attribute__ ((destructor)) static void
on_unload (void)
{
  frida_gadget_unload ();
}

#ifdef HAVE_DARWIN

static void *
frida_gadget_wait_for_permission_to_resume_then_stop_loop (void * user_data)
{
  CFRunLoopRef loop = user_data;

  frida_gadget_wait_for_permission_to_resume ();

  frida_cf_api_try_get ()->CFRunLoopStop (loop);

  return NULL;
}

static void
on_keep_alive_timer_fire (CFRunLoopTimerRef timer, void * info)
{
}

#endif

void
frida_gadget_environment_init (void)
{
  GMemVTable mem_vtable = {
    gum_malloc,
    gum_realloc,
    gum_free,
    gum_calloc,
    gum_malloc,
    gum_realloc
  };

  gum_memory_init ();
  g_mem_set_vtable (&mem_vtable);
  glib_init ();
  g_assertion_set_handler (frida_gadget_on_assert_failure, NULL);
  g_log_set_default_handler (frida_gadget_on_log_message, NULL);
  g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);
#if GLIB_CHECK_VERSION (2, 46, 1)
  gobject_init ();
#endif
  gio_init ();
  gum_init ();
  gum_script_backend_get_type (); /* Warm up */
  frida_error_quark (); /* Initialize early so GDBus will pick it up */

  main_context = g_main_context_ref (g_main_context_default ());
  main_loop = g_main_loop_new (main_context, FALSE);
  main_thread = g_thread_new ("gadget-main-loop", run_main_loop, NULL);
}

void
frida_gadget_environment_deinit (FridaGadgetAutoIgnorer * ignorer)
{
  GSource * source;

  g_assert (main_loop != NULL);

  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_LOW);
  g_source_set_callback (source, stop_main_loop, NULL, NULL);
  g_source_attach (source, main_context);
  g_source_unref (source);

  g_thread_join (main_thread);
  main_thread = NULL;

  g_main_loop_unref (main_loop);
  main_loop = NULL;
  g_main_context_unref (main_context);
  main_context = NULL;

  gio_shutdown ();
  glib_shutdown ();
  frida_gadget_auto_ignorer_shutdown (ignorer);
  g_object_unref (ignorer);
  gum_deinit ();
  gio_deinit ();
  glib_deinit ();
  gum_memory_deinit ();
}

GMainContext *
frida_gadget_environment_get_main_context (void)
{
  return main_context;
}

GumScriptBackend *
frida_gadget_environment_obtain_script_backend (gboolean jit_enabled)
{
  GumScriptBackend * backend = NULL;

#ifdef HAVE_DIET
  backend = gum_script_backend_obtain_duk ();
#else
  if (jit_enabled)
    backend = gum_script_backend_obtain_v8 ();
  if (backend == NULL)
    backend = gum_script_backend_obtain_duk ();
#endif

  return backend;
}

static gpointer
run_main_loop (gpointer data)
{
  (void) data;

  g_main_context_push_thread_default (main_context);
  g_main_loop_run (main_loop);
  g_main_context_pop_thread_default (main_context);

  return NULL;
}

static gboolean
stop_main_loop (gpointer data)
{
  (void) data;

  g_main_loop_quit (main_loop);

  return FALSE;
}

void
frida_gadget_log_info (const gchar * message)
{
  g_info ("%s", message);
}

void
frida_gadget_log_error (const gchar * message)
{
  g_error ("%s", message);
}

static void
frida_gadget_on_assert_failure (const gchar * log_domain, const gchar * file, gint line, const gchar * func, const gchar * message, gpointer user_data)
{
  gchar * full_message;

  while (g_str_has_prefix (file, ".." G_DIR_SEPARATOR_S))
    file += 3;
  if (message == NULL)
    message = "code should not be reached";

  full_message = g_strdup_printf ("%s:%d:%s%s %s", file, line, func, (func[0] != '\0') ? ":" : "", message);
  frida_gadget_on_log_message (log_domain, G_LOG_LEVEL_ERROR, full_message, user_data);
  g_free (full_message);

  abort ();
}

static void
frida_gadget_on_log_message (const gchar * log_domain, GLogLevelFlags log_level, const gchar * message, gpointer user_data)
{
#ifdef HAVE_ANDROID
  int priority;

  (void) user_data;

  switch (log_level & G_LOG_LEVEL_MASK)
  {
    case G_LOG_LEVEL_ERROR:
    case G_LOG_LEVEL_CRITICAL:
    case G_LOG_LEVEL_WARNING:
      priority = ANDROID_LOG_FATAL;
      break;
    case G_LOG_LEVEL_MESSAGE:
    case G_LOG_LEVEL_INFO:
      priority = ANDROID_LOG_INFO;
      break;
    case G_LOG_LEVEL_DEBUG:
      priority = ANDROID_LOG_DEBUG;
      break;
    default:
      g_assert_not_reached ();
  }

  __android_log_write (priority, log_domain, message);
#else
# ifdef HAVE_DARWIN
  FridaCFApi * api = frida_cf_api_try_get ();
  if (api != NULL)
  {
    CFLogLevel cf_log_level;
    CFStringRef message_str, template_str;

    switch (log_level & G_LOG_LEVEL_MASK)
    {
      case G_LOG_LEVEL_ERROR:
        cf_log_level = kCFLogLevelError;
        break;
      case G_LOG_LEVEL_CRITICAL:
        cf_log_level = kCFLogLevelCritical;
        break;
      case G_LOG_LEVEL_WARNING:
        cf_log_level = kCFLogLevelWarning;
        break;
      case G_LOG_LEVEL_MESSAGE:
        cf_log_level = kCFLogLevelNotice;
        break;
      case G_LOG_LEVEL_INFO:
        cf_log_level = kCFLogLevelInfo;
        break;
      case G_LOG_LEVEL_DEBUG:
        cf_log_level = kCFLogLevelDebug;
        break;
      default:
        g_assert_not_reached ();
    }

    message_str = api->CFStringCreateWithCString (NULL, message, kCFStringEncodingUTF8);
    if (log_domain != NULL)
    {
      CFStringRef log_domain_str;

      template_str = api->CFStringCreateWithCString (NULL, "%@: %@", kCFStringEncodingUTF8);
      log_domain_str = api->CFStringCreateWithCString (NULL, log_domain, kCFStringEncodingUTF8);
      api->CFLog (cf_log_level, template_str, log_domain_str, message_str);
      api->CFRelease (log_domain_str);
    }
    else
    {
      template_str = api->CFStringCreateWithCString (NULL, "%@", kCFStringEncodingUTF8);
      api->CFLog (cf_log_level, template_str, message_str);
    }
    api->CFRelease (template_str);
    api->CFRelease (message_str);

    return;
  }
  /* else: fall through to stdout/stderr logging */
# endif

  FILE * file = NULL;
  const gchar * severity = NULL;

  (void) user_data;

  switch (log_level & G_LOG_LEVEL_MASK)
  {
    case G_LOG_LEVEL_ERROR:
      file = stderr;
      severity = "ERROR";
      break;
    case G_LOG_LEVEL_CRITICAL:
      file = stderr;
      severity = "CRITICAL";
      break;
    case G_LOG_LEVEL_WARNING:
      file = stderr;
      severity = "WARNING";
      break;
    case G_LOG_LEVEL_MESSAGE:
      file = stderr;
      severity = "MESSAGE";
      break;
    case G_LOG_LEVEL_INFO:
      file = stdout;
      severity = "INFO";
      break;
    case G_LOG_LEVEL_DEBUG:
      file = stdout;
      severity = "DEBUG";
      break;
    default:
      g_assert_not_reached ();
  }

  fprintf (file, "[%s %s] %s\n", log_domain, severity, message);
  fflush (file);
#endif
}

static void
frida_gadget_auto_ignorer_shutdown (FridaGadgetAutoIgnorer * self)
{
#ifdef G_OS_WIN32
  (void) self;
#else
  GumInterceptor * interceptor = self->interceptor;

  gum_interceptor_revert_function (interceptor, pthread_key_create);

  g_mutex_lock (&self->mutex);
  g_slist_foreach (self->tls_contexts, (GFunc) frida_tls_key_context_free, NULL);
  g_slist_free (self->tls_contexts);
  self->tls_contexts = NULL;
  g_mutex_unlock (&self->mutex);
#endif
}

#ifdef G_OS_WIN32
static uintptr_t
frida_replacement_thread_create (
    void * security,
    unsigned stack_size,
    unsigned (__stdcall * func) (void *),
    void * data,
    unsigned initflag,
    unsigned * thrdaddr)
#else
static int
frida_replacement_thread_create (
    pthread_t * thread,
    const pthread_attr_t * attr,
    void * (* func) (void *),
    void * data)
#endif
{
  GumInvocationContext * ctx;
  FridaGadgetAutoIgnorer * self;

  ctx = gum_interceptor_get_current_invocation ();
  self = FRIDA_GADGET_AUTO_IGNORER (gum_invocation_context_get_replacement_function_data (ctx));

  if (GUM_MEMORY_RANGE_INCLUDES (&self->gadget_range, GUM_ADDRESS (GUM_FUNCPTR_TO_POINTER (func))))
  {
    FridaThreadCreateContext * ctx;

    ctx = g_slice_new (FridaThreadCreateContext);
    ctx->ignorer = g_object_ref (self);
    ctx->thread_func = func;
    ctx->thread_data = data;

    func = frida_thread_create_proxy;
    data = ctx;
  }

#ifdef G_OS_WIN32
  return _beginthreadex (security, stack_size, func, data, initflag, thrdaddr);
#else
  return pthread_create (thread, attr, func, data);
#endif
}

#ifndef G_OS_WIN32

static void
frida_tls_key_context_free (FridaTlsKeyContext * ctx)
{
  if (ctx->replaced)
    gum_interceptor_revert_function (ctx->ignorer->interceptor, ctx->destructor);
  g_object_unref (ctx->ignorer);
  g_slice_free (FridaTlsKeyContext, ctx);
}

static void
frida_replacement_tls_key_destructor (void * data)
{
  GumInvocationContext * ctx;
  FridaTlsKeyContext * tkc;
  GumInterceptor * interceptor;

  ctx = gum_interceptor_get_current_invocation ();
  tkc = gum_invocation_context_get_replacement_function_data (ctx);
  interceptor = tkc->ignorer->interceptor;

  g_object_ref (interceptor);
  gum_interceptor_ignore_current_thread (interceptor);
  tkc->destructor (data);
  gum_interceptor_unignore_current_thread (interceptor);
  g_object_unref (interceptor);
}

static int
frida_replacement_tls_key_create (
    pthread_key_t * key,
    void (* destructor) (void *))
{
  GumInvocationContext * ctx;
  FridaGadgetAutoIgnorer * self;
  GumInterceptor * interceptor;
  int res;

  ctx = gum_interceptor_get_current_invocation ();
  self = FRIDA_GADGET_AUTO_IGNORER (gum_invocation_context_get_replacement_function_data (ctx));
  interceptor = self->interceptor;

  res = pthread_key_create (key, destructor);
  if (res != 0)
    return res;

  if (GUM_MEMORY_RANGE_INCLUDES (&self->gadget_range, GUM_ADDRESS (GUM_FUNCPTR_TO_POINTER (destructor))))
  {
    FridaTlsKeyContext * tkc;

    gum_interceptor_ignore_current_thread (interceptor);

    tkc = g_slice_new (FridaTlsKeyContext);
    tkc->destructor = destructor;
    tkc->replaced = FALSE;

    tkc->ignorer = g_object_ref (self);

    if (gum_interceptor_replace_function (interceptor, destructor, frida_replacement_tls_key_destructor, tkc) == GUM_REPLACE_OK)
    {
      tkc->replaced = TRUE;

      g_mutex_lock (&self->mutex);
      self->tls_contexts = g_slist_prepend (self->tls_contexts, tkc);
      g_mutex_unlock (&self->mutex);
    }
    else
    {
      frida_tls_key_context_free (tkc);
    }

    gum_interceptor_unignore_current_thread (interceptor);
  }

  return 0;
}

#endif

void
frida_gadget_auto_ignorer_replace_apis (FridaGadgetAutoIgnorer * self)
{
  gum_interceptor_begin_transaction (self->interceptor);

  gum_interceptor_replace_function (self->interceptor,
      frida_get_address_of_thread_create_func (),
      GUM_FUNCPTR_TO_POINTER (frida_replacement_thread_create),
      self);

#ifndef G_OS_WIN32
  gum_interceptor_replace_function (self->interceptor,
      pthread_key_create,
      GUM_FUNCPTR_TO_POINTER (frida_replacement_tls_key_create),
      self);
#endif

  gum_interceptor_end_transaction (self->interceptor);
}

void
frida_gadget_auto_ignorer_revert_apis (FridaGadgetAutoIgnorer * self)
{
  gum_interceptor_revert_function (self->interceptor, frida_get_address_of_thread_create_func ());
}

static gpointer
frida_get_address_of_thread_create_func (void)
{
#if defined (G_OS_WIN32)
  return GUM_FUNCPTR_TO_POINTER (_beginthreadex);
#elif defined (HAVE_DARWIN)
  return GUM_FUNCPTR_TO_POINTER (pthread_create);
#else
  static gsize gonce_value = 0;

  if (g_once_init_enter (&gonce_value))
  {
    pthread_t linker_stub_warmup_thread;

    pthread_create (&linker_stub_warmup_thread, NULL, frida_linker_stub_warmup_thread, NULL);
    pthread_detach (linker_stub_warmup_thread);

    g_once_init_leave (&gonce_value, TRUE);
  }

  return GUM_FUNCPTR_TO_POINTER (pthread_create);
#endif
}

static NativeThreadFuncReturnType NATIVE_THREAD_FUNC_API
frida_thread_create_proxy (void * data)
{
  FridaThreadCreateContext * ctx = data;

  gum_script_backend_ignore (gum_process_get_current_thread_id ());

  /* This allows us to free the data no matter how the thread exits */
  g_private_set (&frida_thread_create_context_key, ctx);

  return ctx->thread_func (ctx->thread_data);
}

static void
frida_thread_create_context_free (FridaThreadCreateContext * ctx)
{
  g_object_unref (ctx->ignorer);
  g_slice_free (FridaThreadCreateContext, ctx);

  gum_script_backend_unignore_later (gum_process_get_current_thread_id ());
}

static void *
frida_linker_stub_warmup_thread (void * data)
{
  (void) data;

  return NULL;
}

#ifdef HAVE_DARWIN

static FridaCFApi *
frida_cf_api_try_get (void)
{
  static gsize api_value = 0;
  FridaCFApi * api;

  if (g_once_init_enter (&api_value))
  {
    const gchar * cf_path = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
    void * cf;

    /*
     * CoreFoundation must be loaded by the main thread, so we should avoid loading it.
     */
    if (gum_module_find_base_address (cf_path) != 0)
    {
      cf = dlopen (cf_path, RTLD_LAZY | RTLD_GLOBAL);
      g_assert (cf != NULL);

      api = g_slice_new (FridaCFApi);

#define FRIDA_ASSIGN_CF_SYMBOL(n) \
    api->n = dlsym (cf, G_STRINGIFY (n)); \
    g_assert (api->n != NULL)

      FRIDA_ASSIGN_CF_SYMBOL (kCFAllocatorDefault);
      FRIDA_ASSIGN_CF_SYMBOL (kCFRunLoopCommonModes);

      FRIDA_ASSIGN_CF_SYMBOL (CFRelease);

      FRIDA_ASSIGN_CF_SYMBOL (CFStringCreateWithCString);

      FRIDA_ASSIGN_CF_SYMBOL (CFRunLoopGetMain);
      FRIDA_ASSIGN_CF_SYMBOL (CFRunLoopRun);
      FRIDA_ASSIGN_CF_SYMBOL (CFRunLoopStop);
      FRIDA_ASSIGN_CF_SYMBOL (CFRunLoopTimerCreate);
      FRIDA_ASSIGN_CF_SYMBOL (CFRunLoopAddTimer);
      FRIDA_ASSIGN_CF_SYMBOL (CFRunLoopTimerInvalidate);

      FRIDA_ASSIGN_CF_SYMBOL (CFLog);

#undef FRIDA_ASSIGN_CF_SYMBOL

      dlclose (cf);
    }
    else
    {
      api = NULL;
    }

    g_once_init_leave (&api_value, 1 + GPOINTER_TO_SIZE (api));
  }

  api = GSIZE_TO_POINTER (api_value - 1);

  return api;
}

#endif
