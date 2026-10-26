#include "Trash.hpp"
#import <Foundation/Foundation.h>

namespace Slic3r::ModelLibrary {
bool move_to_trash(const std::filesystem::path& path)
{
    @autoreleasepool {
        const auto name = path.u8string();
        NSString* value = [[NSString alloc] initWithBytes:name.data() length:name.size() encoding:NSUTF8StringEncoding];
        if (!value) return false;
        NSURL* url = [NSURL fileURLWithPath:value];
        const bool moved = [[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:nil error:nil];
        [value release];
        return moved;
    }
}
}
