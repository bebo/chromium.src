
#ifndef MEDIA_DIREACT_SHOW_H_
#define MEDIA_DIREACT_SHOW_H_

#include <windows.h>

#include <memory>

#include "base/macros.h"
#include "media/base/media_export.h"

namespace base {
  template <typename Type>
  struct StaticMemorySingletonTraits;
}
namespace media {

class MEDIA_EXPORT DirectShow {
 public:
  static DirectShow* GetInstance();
  void hi();


 private:
  DirectShow();
  virtual ~DirectShow();
  friend struct base::StaticMemorySingletonTraits<DirectShow>;
  DISALLOW_COPY_AND_ASSIGN(DirectShow);
};

}  // namespace media

#endif  // MEDIA_DIREACT_SHOW_H_
