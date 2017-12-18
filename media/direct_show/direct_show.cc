
#include "media/direct_show/direct_show.h"

#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/singleton.h"

namespace media {

  DirectShow * DirectShow::GetInstance() {
    return base::Singleton<DirectShow, base::StaticMemorySingletonTraits<
                                                 DirectShow>>::get();
  }

  void DirectShow::hi() {
    LOG(INFO) << __func__ ;
  }

  DirectShow::DirectShow() {
    LOG(INFO) << __func__ ;
  }

  DirectShow::~DirectShow() {
    LOG(INFO) << __func__ ;
  }

}
