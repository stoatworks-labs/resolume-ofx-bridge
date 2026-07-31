//
// OFX Bridge - a window around the generator.
//
// The app does no work of its own: it collects two paths and calls
// ofxgen::generate, the same entry point the ofxgen CLI uses. Anything that
// changes what a generated bundle contains belongs in source/gen/Generator.cpp,
// not here.
//

#import "BridgeWindow.h"

#import <Cocoa/Cocoa.h>

@interface AppDelegate : NSObject < NSApplicationDelegate >
@property( strong ) BridgeWindow* controller;
@end

@implementation AppDelegate

- ( void )applicationDidFinishLaunching:( NSNotification* )notification
{
	self.controller = [BridgeWindow create];
	[NSApp activateIgnoringOtherApps:YES];
}

- ( BOOL )applicationShouldTerminateAfterLastWindowClosed:( NSApplication* )app
{
	return YES;
}

@end

/// Built in code because there is no nib: without a menu there is no ⌘Q, and
/// the standard Edit items are what make the log copyable.
static NSMenu* buildMenu()
{
	NSMenu* mainMenu = [[NSMenu alloc] init];

	NSMenuItem* appItem = [[NSMenuItem alloc] init];
	[mainMenu addItem:appItem];
	NSMenu* appMenu = [[NSMenu alloc] init];
	[appMenu addItemWithTitle:@"About OFX Bridge" action:@selector( orderFrontStandardAboutPanel: ) keyEquivalent:@""];
	[appMenu addItem:[NSMenuItem separatorItem]];
	[appMenu addItemWithTitle:@"Hide OFX Bridge" action:@selector( hide: ) keyEquivalent:@"h"];
	[appMenu addItemWithTitle:@"Quit OFX Bridge" action:@selector( terminate: ) keyEquivalent:@"q"];
	appItem.submenu = appMenu;

	NSMenuItem* editItem = [[NSMenuItem alloc] init];
	[mainMenu addItem:editItem];
	NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
	[editMenu addItemWithTitle:@"Cut" action:@selector( cut: ) keyEquivalent:@"x"];
	[editMenu addItemWithTitle:@"Copy" action:@selector( copy: ) keyEquivalent:@"c"];
	[editMenu addItemWithTitle:@"Paste" action:@selector( paste: ) keyEquivalent:@"v"];
	[editMenu addItemWithTitle:@"Select All" action:@selector( selectAll: ) keyEquivalent:@"a"];
	editItem.submenu = editMenu;

	return mainMenu;
}

int main( int argc, const char** argv )
{
	@autoreleasepool
	{
		NSApplication* app = [NSApplication sharedApplication];
		app.activationPolicy = NSApplicationActivationPolicyRegular;
		app.mainMenu         = buildMenu();

		AppDelegate* delegate = [[AppDelegate alloc] init];
		app.delegate          = delegate;
		[app run];
	}
	return 0;
}
