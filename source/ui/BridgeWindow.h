#pragma once
//
// The generator's window. Built in code rather than from a nib, so the whole UI
// is one reviewable file and the build stays a plain CMake target.
//

#import <Cocoa/Cocoa.h>

@interface BridgeWindow : NSWindowController

/// Creates the window, restores the last-used paths, and shows it.
+ ( instancetype )create;

@end
