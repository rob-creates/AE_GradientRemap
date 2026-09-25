#import "GradientRemap_FileDialog.h"

#import <Cocoa/Cocoa.h>

// Deliberately not restricting to a specific UTType/allowedFileTypes: the old
// allowedFileTypes API is deprecated on newer macOS SDKs, and the replacement
// (allowedContentTypes, UniformTypeIdentifiers) needs macOS 11+ -- this project has no
// stated minimum-macOS target, so avoiding an SDK-version dependency for a "pick a CSV
// file" panel outweighs the small convenience of pre-filtering the file list. A wrong
// file just fails ParseGradientKnotsCSV with a clear error message instead.

namespace GradientRemap {

bool ShowSaveCSVPanel(std::string* out_path) {
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = @"Gradient.csv";
        panel.canCreateDirectories = YES;

        NSModalResponse result = [panel runModal];
        if (result != NSModalResponseOK || !panel.URL) return false;

        NSString* path = panel.URL.path;
        if (![[path.pathExtension lowercaseString] isEqualToString:@"csv"]) {
            path = [path stringByAppendingPathExtension:@"csv"];
        }
        *out_path = std::string(path.UTF8String);
        return true;
    }
}

bool ShowOpenCSVPanel(std::string* out_path) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;

        NSModalResponse result = [panel runModal];
        if (result != NSModalResponseOK || panel.URLs.count == 0) return false;

        *out_path = std::string(panel.URLs[0].path.UTF8String);
        return true;
    }
}

} // namespace GradientRemap
