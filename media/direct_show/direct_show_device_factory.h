#ifndef MEDIA_DIRECT_SHOW_DEVICE_FACTORY_H_
#define MEDIA_DIRECT_SHOW_DEVICE_FACTORY_H_

/* #include <windows.h> */

#include <memory>
#include <string>
#include <vector>

#include "base/macros.h"
#include "media/base/media_export.h"
#include "media/direct_show/direct_show.h"
#include "media/direct_show/capability_list_win.h"

namespace base {
  template <typename Type>
  struct StaticMemorySingletonTraits;
}

namespace media {

  enum class MEDIA_EXPORT DirectShowType { Audio, Video };


  class MEDIA_EXPORT DirectShowDeviceDescriptor {
    public:
      DirectShowDeviceDescriptor(
        const std::string& display_name,
        const std::string& device_id,
        const std::string& model_id);
      ~DirectShowDeviceDescriptor();

      std::string display_name;  // Name that is intended for display in the UI
      std::string device_id;
      // A unique hardware identifier of the capture device.
      // It is of the form "[vid]:[pid]" when a USB device is detected, and empty
      // otherwise.
      std::string model_id;
  };

  using DirectShowDeviceDescriptors = std::vector<DirectShowDeviceDescriptor>;

  class MEDIA_EXPORT DirectShowDeviceFactory {
    public:
      static DirectShowDeviceFactory* GetInstance();
      void GetDeviceDescriptors(DirectShowType type, DirectShowDeviceDescriptors* device_descriptors);
      void GetDeviceCapabilityList(DirectShowType type,
              std::string device_id,
              DirectShowDeviceCapabilityList* device_capablity_list);
      bool IsDirectShowDevice(std::string device_id);
      DirectShow *GetController(std::string device_id);

    private:
      DirectShowDeviceFactory();
      virtual ~DirectShowDeviceFactory();
      friend struct base::StaticMemorySingletonTraits<DirectShowDeviceFactory>;

      DirectShowDeviceDescriptors device_descriptors_;
      DirectShow* fake_list_;

      DISALLOW_COPY_AND_ASSIGN(DirectShowDeviceFactory);
  };

}

#endif // MEDIA_DIRECT_SHOW_DEVICE_FACTORY_H_



