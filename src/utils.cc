// Copyright 2011 Google Inc. All Rights Reserved.
// Author: enh@google.com (Elliott Hughes)

#include "utils.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "UniquePtr.h"
#include "file.h"
#include "object.h"
#include "os.h"

#if defined(HAVE_PRCTL)
#include <sys/prctl.h>
#endif

namespace art {

bool ReadFileToString(const std::string& file_name, std::string* result) {
  UniquePtr<File> file(OS::OpenFile(file_name.c_str(), false));
  if (file.get() == NULL) {
    return false;
  }

  char buf[8 * KB];
  while (true) {
    int64_t n = file->Read(buf, sizeof(buf));
    if (n == -1) {
      return false;
    }
    if (n == 0) {
      return true;
    }
    result->append(buf, n);
  }
}

std::string GetIsoDate() {
  time_t now = time(NULL);
  struct tm tmbuf;
  struct tm* ptm = localtime_r(&now, &tmbuf);
  return StringPrintf("%04d-%02d-%02d %02d:%02d:%02d",
      ptm->tm_year + 1900, ptm->tm_mon+1, ptm->tm_mday,
      ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
}

std::string PrettyDescriptor(const String* java_descriptor) {
  std::string descriptor(java_descriptor->ToModifiedUtf8());

  // Count the number of '['s to get the dimensionality.
  const char* c = descriptor.c_str();
  size_t dim = 0;
  while (*c == '[') {
    dim++;
    c++;
  }

  // Reference or primitive?
  if (*c == 'L') {
    // "[[La/b/C;" -> "a.b.C[][]".
    c++; // Skip the 'L'.
  } else {
    // "[[B" -> "byte[][]".
    // To make life easier, we make primitives look like unqualified
    // reference types.
    switch (*c) {
    case 'B': c = "byte;"; break;
    case 'C': c = "char;"; break;
    case 'D': c = "double;"; break;
    case 'F': c = "float;"; break;
    case 'I': c = "int;"; break;
    case 'J': c = "long;"; break;
    case 'S': c = "short;"; break;
    case 'Z': c = "boolean;"; break;
    default: return descriptor;
    }
  }

  // At this point, 'c' is a string of the form "fully/qualified/Type;"
  // or "primitive;". Rewrite the type with '.' instead of '/':
  std::string result;
  const char* p = c;
  while (*p != ';') {
    char ch = *p++;
    if (ch == '/') {
      ch = '.';
    }
    result.push_back(ch);
  }
  // ...and replace the semicolon with 'dim' "[]" pairs:
  while (dim--) {
    result += "[]";
  }
  return result;
}

std::string PrettyField(const Field* f) {
  if (f == NULL) {
    return "null";
  }
  std::string result(PrettyDescriptor(f->GetDeclaringClass()->GetDescriptor()));
  result += '.';
  result += f->GetName()->ToModifiedUtf8();
  return result;
}

std::string PrettyMethod(const Method* m, bool with_signature) {
  if (m == NULL) {
    return "null";
  }
  Class* c = m->GetDeclaringClass();
  std::string result(PrettyDescriptor(c->GetDescriptor()));
  result += '.';
  result += m->GetName()->ToModifiedUtf8();
  if (with_signature) {
    // TODO: iterate over the signature's elements and pass them all to
    // PrettyDescriptor? We'd need to pull out the return type specially, too.
    result += m->GetSignature()->ToModifiedUtf8();
  }
  return result;
}

std::string PrettyType(const Object* obj) {
  if (obj == NULL) {
    return "null";
  }
  if (obj->GetClass() == NULL) {
    return "(raw)";
  }
  std::string result(PrettyDescriptor(obj->GetClass()->GetDescriptor()));
  if (obj->IsClass()) {
    result += "<" + PrettyDescriptor(obj->AsClass()->GetDescriptor()) + ">";
  }
  return result;
}

std::string MangleForJni(const std::string& s) {
  std::string result;
  size_t char_count = CountModifiedUtf8Chars(s.c_str());
  const char* cp = &s[0];
  for (size_t i = 0; i < char_count; ++i) {
    uint16_t ch = GetUtf16FromUtf8(&cp);
    if (ch == '$' || ch > 127) {
      StringAppendF(&result, "_0%04x", ch);
    } else {
      switch (ch) {
      case '_':
        result += "_1";
        break;
      case ';':
        result += "_2";
        break;
      case '[':
        result += "_3";
        break;
      case '/':
        result += "_";
        break;
      default:
        result.push_back(ch);
        break;
      }
    }
  }
  return result;
}

std::string JniShortName(const Method* m) {
  Class* declaring_class = m->GetDeclaringClass();

  std::string class_name(declaring_class->GetDescriptor()->ToModifiedUtf8());
  // Remove the leading 'L' and trailing ';'...
  CHECK(class_name[0] == 'L') << class_name;
  CHECK(class_name[class_name.size() - 1] == ';') << class_name;
  class_name.erase(0, 1);
  class_name.erase(class_name.size() - 1, 1);

  std::string method_name(m->GetName()->ToModifiedUtf8());

  std::string short_name;
  short_name += "Java_";
  short_name += MangleForJni(class_name);
  short_name += "_";
  short_name += MangleForJni(method_name);
  return short_name;
}

std::string JniLongName(const Method* m) {
  std::string long_name;
  long_name += JniShortName(m);
  long_name += "__";

  std::string signature(m->GetSignature()->ToModifiedUtf8());
  signature.erase(0, 1);
  signature.erase(signature.begin() + signature.find(')'), signature.end());

  long_name += MangleForJni(signature);

  return long_name;
}

void Split(const std::string& s, char delim, std::vector<std::string>& result) {
  const char* p = s.data();
  const char* end = p + s.size();
  while (p != end) {
    if (*p == delim) {
      ++p;
    } else {
      const char* start = p;
      while (++p != end && *p != delim) {
        // Skip to the next occurrence of the delimiter.
      }
      result.push_back(std::string(start, p - start));
    }
  }
}

void SetThreadName(const char *threadName) {
  int hasAt = 0;
  int hasDot = 0;
  const char *s = threadName;
  while (*s) {
    if (*s == '.') {
      hasDot = 1;
    } else if (*s == '@') {
      hasAt = 1;
    }
    s++;
  }
  int len = s - threadName;
  if (len < 15 || hasAt || !hasDot) {
    s = threadName;
  } else {
    s = threadName + len - 15;
  }
#if defined(HAVE_ANDROID_PTHREAD_SETNAME_NP)
  /* pthread_setname_np fails rather than truncating long strings */
  char buf[16];       // MAX_TASK_COMM_LEN=16 is hard-coded into bionic
  strncpy(buf, s, sizeof(buf)-1);
  buf[sizeof(buf)-1] = '\0';
  errno = pthread_setname_np(pthread_self(), buf);
  if (errno != 0) {
    PLOG(WARNING) << "Unable to set the name of current thread to '" << buf << "'";
  }
#elif defined(HAVE_PRCTL)
  prctl(PR_SET_NAME, (unsigned long) s, 0, 0, 0);
#else
#error no implementation for SetThreadName
#endif
}

pid_t GetOwner(pthread_mutex_t* mutex) {
#ifdef __BIONIC__
  return static_cast<pid_t>(((mutex)->value >> 16) & 0xffff);
#else
  UNIMPLEMENTED(FATAL);
  return 0;
#endif
}

}  // namespace art

// Neither bionic nor glibc exposes gettid(2).
#define __KERNEL__
#include <linux/unistd.h>
namespace art {
#ifdef _syscall0
_syscall0(pid_t, GetTid)
#else
pid_t GetTid() { return syscall(__NR_gettid); }
#endif
}  // namespace art
#undef __KERNEL__
