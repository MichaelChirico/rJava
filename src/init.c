#include "rJava.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* global variables */
JavaVM *jvm;

/* cached, global objects */
jclass javaStringClass;
jclass javaObjectClass;
jclass javaClassClass;
jclass javaFieldClass;

/* cached, global method IDs */
jmethodID mid_forName;
jmethodID mid_getName;
jmethodID mid_getType;
jmethodID mid_getField;

int rJava_initialized = 0;


static int    jvm_opts=0;
static char **jvm_optv=0;

#ifdef JNI_VERSION_1_2 
static JavaVMOption *vm_options;
static JavaVMInitArgs vm_args;
#else  
#error "Java/JNI 1.2 or higher is required!"
#endif

#define H_OUT  1
#define H_EXIT 2

#ifdef Win32
/* the hooks are reportedly causing problems on Windows, so disable them by default */
static int default_hooks = 0;
#else
static int default_hooks = H_OUT|H_EXIT;
#endif

static int JNICALL vfprintf_hook(FILE *f, const char *fmt, va_list ap) {
#ifdef Win32
  /* on Windows f doesn't refer to neither stderr nor stdout,
     so we have no way of finding out the target, so we assume stderr */
  REvprintf(fmt, ap);
  return 0;
#else
  if (f==stderr) {
    REvprintf(fmt, ap);
    return 0;
  } else if (f==stdout) {
    Rvprintf(fmt, ap);
    return 0;
  }
  return vfprintf(f, fmt, ap);
#endif
}

static void JNICALL exit_hook(int status) {
  REprintf("\nJava requested System.exit(%d), closing R.\n", status);
  /* FIXME: we could do something smart here such as running a call-back
     into R ... jump into R event loop ... at any rate we cannot return,
     but we don't want to kill R ... */
  exit(status);
}

static int initJVM(const char *user_classpath, int opts, char **optv, int hooks) {
  int total_num_properties, propNum = 0;
  jint res;
  char *classpath;
  
  if(!user_classpath)
    /* use the CLASSPATH environment variable as default */
    user_classpath = getenv("CLASSPATH");
  if(!user_classpath) user_classpath = "";
  
  vm_args.version = JNI_VERSION_1_2;
  if(JNI_GetDefaultJavaVMInitArgs(&vm_args) != JNI_OK) {
    error("JNI 1.2 or higher is required");
    return -1;    
  }
    
  /* leave room for class.path, and optional jni args */
  total_num_properties = 6 + opts;
    
  vm_options = (JavaVMOption *) calloc(total_num_properties, sizeof(JavaVMOption));
  vm_args.version = JNI_VERSION_1_2; /* should we do that or keep the default? */
  vm_args.options = vm_options;
  vm_args.ignoreUnrecognized = JNI_TRUE;
  
  classpath = (char*) calloc(24 + strlen(user_classpath), sizeof(char));
  sprintf(classpath, "-Djava.class.path=%s", user_classpath);
  
  vm_options[propNum++].optionString = classpath;   
  
  /*   print JNI-related messages */
  /* vm_options[propNum++].optionString = "-verbose:class,jni"; */
  
  if (optv) {
    int i=0;
    while (i<opts) {
      if (optv[i]) vm_options[propNum++].optionString = optv[i];
      i++;
    }
  }
  if (hooks&H_OUT) {
    vm_options[propNum].optionString = "vfprintf";
    vm_options[propNum++].extraInfo  = vfprintf_hook;
  }
  if (hooks&H_EXIT) {
    vm_options[propNum].optionString = "exit";
    vm_options[propNum++].extraInfo  = exit_hook;
  }
  vm_args.nOptions = propNum;

  /* Create the Java VM */
  res = JNI_CreateJavaVM(&jvm,(void **)&eenv, &vm_args);
  
  if (res != 0)
    error("Cannot create Java virtual machine (%d)", res);
  if (!eenv)
    error("Cannot obtain JVM environemnt");
  return 0;
}

#ifdef THREADS
#include <pthread.h>

#ifdef XXDARWIN
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFRunLoop.h>
#endif

pthread_t initJVMpt;
pthread_mutex_t initMutex = PTHREAD_MUTEX_INITIALIZER;

int thInitResult = 0;
int initAWT = 0;

static void *initJVMthread(void *classpath)
{
  int ws;
  jclass c;
  JNIEnv *lenv;

  thInitResult=initJVM((char*)classpath, jvm_opts, jvm_optv, default_hooks);
  if (thInitResult) return 0;

  init_rJava();

  lenv = eenv; /* we make a local copy before unlocking just in case
		  someone messes with it before we can use it */

  _dbg(rjprintf("initJVMthread: unlocking\n"));
  pthread_mutex_unlock(&initMutex);

  if (initAWT) {
    _dbg(rjprintf("initJVMthread: get AWT class\n"));
    /* we are still on the same thread, so we can safely use eenv */
    c = (*lenv)->FindClass(lenv, "java/awt/Frame");
  }

  _dbg(rjprintf("initJVMthread: returning from the init thread.\n"));
  return 0;
}

#endif



/* initialize internal structures/variables of rJava.
   The JVM initialization was performed before (but may have failed)
*/
HIDE void init_rJava(void) {
  jclass c;
  JNIEnv *env = getJNIEnv();
  if (!env) return; /* initJVMfailed, so we cannot proceed */
  
  /* get global classes. we make the references explicitely global (although unloading of String/Object is more than unlikely) */
  c=(*env)->FindClass(env, "java/lang/String");
  if (!c) error("unable to find the basic String class");
  javaStringClass=(*env)->NewGlobalRef(env, c);
  if (!javaStringClass) error("unable to create a global reference to the basic String class");
  (*env)->DeleteLocalRef(env, c);

  c=(*env)->FindClass(env, "java/lang/Object");
  if (!c) error("unable to find the basic Object class");
  javaObjectClass=(*env)->NewGlobalRef(env, c);
  if (!javaObjectClass) error("unable to create a global reference to the basic Object class");
  (*env)->DeleteLocalRef(env, c);

  c = (*env)->FindClass(env, "java/lang/Class");
  if (!c) error("unable to find the basic Class class");
  javaClassClass=(*env)->NewGlobalRef(env, c);
  if (!javaClassClass) error("unable to create a global reference to the basic Class class");
  (*env)->DeleteLocalRef(env, c);

  c = (*env)->FindClass(env, "java/lang/reflect/Field");
  if (!c) error("unable to find the basic Field class");
  javaFieldClass=(*env)->NewGlobalRef(env, c);
  if (!javaFieldClass) error("unable to create a global reference to the basic Class class");
  (*env)->DeleteLocalRef(env, c);

  mid_forName  = (*env)->GetStaticMethodID(env, javaClassClass, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
  if (!mid_forName) error("cannot obtain Class.forName method ID");
  mid_getName  = (*env)->GetMethodID(env, javaClassClass, "getName", "()Ljava/lang/String;");
  if (!mid_getName) error("cannot obtain Class.getName method ID");
  mid_getField = (*env)->GetMethodID(env, javaClassClass, "getField",
				     "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
  if (!mid_getField) error("cannot obtain Class.getField method ID");
  mid_getType  = (*env)->GetMethodID(env, javaFieldClass, "getType",
				     "()Ljava/lang/Class;");
  if (!mid_getType) error("cannot obtain Field.getType method ID");

  rJava_initialized = 1;
}

/** RinitJVM(classpath)
    initializes JVM with the specified class path */
REP SEXP RinitJVM(SEXP par)
{
  const char *c=0;
  SEXP e=CADR(par);
  int r=0;
  JavaVM *jvms[32];
  jsize vms=0;
  
  jvm_opts=0;
  jvm_optv=0;
  
  if (TYPEOF(e)==STRSXP && LENGTH(e)>0)
	  c=CHAR(STRING_ELT(e,0));

  e = CADDR(par);
  if (TYPEOF(e)==STRSXP && LENGTH(e)>0) {
	  int len = LENGTH(e);
	  jvm_optv=(char**)malloc(sizeof(char*)*len);
	  while (jvm_opts < len) {
		  jvm_optv[jvm_opts] = strdup(CHAR(STRING_ELT(e, jvm_opts)));
		  jvm_opts++;
	  }
  }
  
  r=JNI_GetCreatedJavaVMs(jvms, 32, &vms);
  if (r) {
    Rf_error("JNI_GetCreatedJavaVMs returned %d\n", r);
  } else {
    if (vms>0) {
      int i=0;
      _dbg(rjprintf("RinitJVM: Total %d JVMs found. Trying to attach the current thread.\n", (int)vms));
      while (i<vms) {
	if (jvms[i]) {
	  if (!(*jvms[i])->AttachCurrentThread(jvms[i], (void**)&eenv, NULL)) {
            _dbg(rjprintf("RinitJVM: Attached to existing JVM #%d.\n", i+1));
	    break;
	  }
	}
	i++;
      }
      if (i==vms) Rf_error("Failed to attach to any existing JVM.");
      else {
	jvm = jvms[i];
	init_rJava();
      }
      PROTECT(e=allocVector(INTSXP,1));
      INTEGER(e)[0]=(i==vms)?-2:1;
      UNPROTECT(1);
      return e;
    }
  }

#ifdef THREADS
  if (getenv("R_GUI_APP_VERSION") || getenv("RJAVA_INIT_AWT"))
    initAWT=1;

  _dbg(rjprintf("RinitJVM(threads): launching thread\n"));
  pthread_mutex_lock(&initMutex);
  pthread_create(&initJVMpt, 0, initJVMthread, c);
  _dbg(rjprintf("RinitJVM(threads): waiting for mutex\n"));
  pthread_mutex_lock(&initMutex);
  pthread_mutex_unlock(&initMutex);
  /* pthread_join(initJVMpt, 0); */
  _dbg(rjprintf("RinitJVM(threads): attach\n"));
  /* since JVM was initialized by another thread, we need to attach ourselves */
  (*jvm)->AttachCurrentThread(jvm, (void**)&eenv, NULL);
  _dbg(rjprintf("RinitJVM(threads): done.\n"));
  r = thInitResult;
#else
  profStart();
  r=initJVM(c, jvm_opts, jvm_optv, default_hooks);
  init_rJava();
  _prof(profReport("init_rJava:"));
  _dbg(rjprintf("RinitJVM(non-threaded): initJVM returned %d\n", r));
#endif
  if (jvm_optv) free(jvm_optv);
  jvm_opts=0;
  PROTECT(e=allocVector(INTSXP,1));
  INTEGER(e)[0]=r;
  UNPROTECT(1);
  return e;
}

REP void doneJVM() {
  (*jvm)->DestroyJavaVM(jvm);
  jvm = 0;
  eenv = 0;
}

