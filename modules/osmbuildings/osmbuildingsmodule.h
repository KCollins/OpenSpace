#ifndef __OPENSPACE_MODULE_OSMBUILDINGS___OSMBUILDINGSMODULE___H__
#define __OPENSPACE_MODULE_OSMBUILDINGS___OSMBUILDINGSMODULE___H__

#include <openspace/util/openspacemodule.h>

namespace openspace {

class OsmBuildingsModule : public OpenSpaceModule {
public:
    constexpr static const char* Name = "OsmBuildings";
    OsmBuildingsModule();

protected:
    void internalInitialize(const Dictionary& dictionary) override;
};

} // namespace openspace

#endif // __OPENSPACE_MODULE_OSMBUILDINGS___OSMBUILDINGSMODULE___H__