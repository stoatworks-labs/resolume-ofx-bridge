#import "BridgeWindow.h"

#include "../gen/Generator.h"
#include "../ofxbridge/Catalog.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace {

NSString* const kDefaultsSource = @"sourcePath";
NSString* const kDefaultsDest   = @"destPath";

/// How a log line is coloured. The generator emits plain text; the classification
/// happens here so the CLI and the GUI stay on the same log strings.
enum class LogKind
{
	Info,
	Heading,
	Good,
	Warn,
	Bad
};

NSArray< NSString* >* resolumeProductNames()
{
	return @[ @"Arena", @"Avenue", @"Alley", @"Wire" ];
}

/// What was actually checked, rather than what seems likely: only Arena and
/// Avenue scan an "Extra Effects" folder. That string is present in the Arena
/// binary and absent from Alley's and Wire's, even though all three link the
/// same FFGL engine. Buttons still appear for the others — installing there is
/// harmless, and it is where they would look if support arrives — but clicking
/// one says so rather than implying the plugins will show up.
bool productHostsEffects( NSString* name )
{
	return [name isEqualToString:@"Arena"] || [name isEqualToString:@"Avenue"];
}

/// A product counts as installed if either its application folder or its
/// documents folder exists — the documents folder is the one we write into, and
/// it survives the app being moved or removed.
NSArray< NSString* >* installedResolumeProducts()
{
	NSFileManager* fm         = [NSFileManager defaultManager];
	NSString* documents       = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents"];
	NSMutableArray* installed = [NSMutableArray array];

	for( NSString* name in resolumeProductNames() )
	{
		NSString* app  = [NSString stringWithFormat:@"/Applications/Resolume %@", name];
		NSString* docs = [documents stringByAppendingPathComponent:[NSString stringWithFormat:@"Resolume %@", name]];
		if( [fm fileExistsAtPath:app] || [fm fileExistsAtPath:docs] )
			[installed addObject:name];
	}
	return installed;
}

NSString* extraEffectsPath( NSString* product )
{
	return [NSHomeDirectory()
		stringByAppendingPathComponent:[NSString stringWithFormat:@"Documents/Resolume %@/Extra Effects", product]];
}

/// Paths are shown with `~` for the home directory, as the rest of macOS does.
/// The field and the log are read over people's shoulders and appear in
/// screenshots, and the account name is nobody else's business.
NSString* forDisplay( NSString* path )
{
	return [path stringByAbbreviatingWithTildeInPath];
}

/// The inverse, applied to whatever is in a field before it is used. Typing a
/// `~` path by hand has to work too, so this is not merely undoing the above.
NSString* forUse( NSString* path )
{
	return [path stringByExpandingTildeInPath];
}

NSTextField* makeLabel( NSString* text )
{
	NSTextField* label                              = [NSTextField labelWithString:text];
	label.translatesAutoresizingMaskIntoConstraints = NO;
	return label;
}

} // namespace

@interface BridgeWindow () < NSWindowDelegate >

@property( strong ) NSTextField* sourceField;
@property( strong ) NSTextField* destField;
@property( strong ) NSButton* startButton;
@property( strong ) NSButton* cancelButton;
@property( strong ) NSButton* revealButton;
@property( strong ) NSProgressIndicator* progress;
@property( strong ) NSTextField* statusLabel;
@property( strong ) NSTextView* logView;
@property( strong ) NSFont* logFont;
@property( strong ) NSArray< NSString* >* quickDestinations;

- ( void )buildInterface;
- ( void )appendLine:( NSString* )line kind:( LogKind )kind;
- ( void )setProgressDone:( int )done total:( int )total label:( NSString* )label;
- ( void )finishWithGenerated:( int )generated
					  skipped:( int )skipped
					cancelled:( BOOL )cancelled
						error:( NSString* )error;
+ ( LogKind )kindForLine:( NSString* )line;

@end

@implementation BridgeWindow
{
	// Read from the worker thread, set from the main thread.
	std::shared_ptr< std::atomic< bool > > _cancelFlag;
}

+ ( instancetype )create
{
	NSWindow* window = [[NSWindow alloc]
		initWithContentRect:NSMakeRect( 0, 0, 780, 640 )
				  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
							NSWindowStyleMaskResizable
					backing:NSBackingStoreBuffered
					  defer:NO];
	window.title    = @"OFX Bridge";
	window.minSize  = NSMakeSize( 620, 480 );

	BridgeWindow* controller = [[BridgeWindow alloc] initWithWindow:window];
	window.delegate          = controller;
	[controller buildInterface];
	[window center];
	[controller showWindow:nil];
	[window makeKeyAndOrderFront:nil];
	return controller;
}

#pragma mark - Interface

- ( void )buildInterface
{
	self.logFont = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];

	NSView* content = self.window.contentView;

	// --- source -----------------------------------------------------------
	NSTextField* sourceLabel = makeLabel( @"OFX plugins" );

	self.sourceField                                        = [[NSTextField alloc] init];
	self.sourceField.translatesAutoresizingMaskIntoConstraints = NO;
	self.sourceField.placeholderString = @"Default plugin locations (/Library/OFX/Plugins, ~/Library/OFX/Plugins, "
										 @"$OFX_PLUGIN_PATH)";
	self.sourceField.stringValue = [[NSUserDefaults standardUserDefaults] stringForKey:kDefaultsSource] ?: @"";

	NSButton* chooseSource = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector( chooseSource: )];
	NSButton* clearSource  = [NSButton buttonWithTitle:@"Use defaults"
												target:self
												action:@selector( useDefaultLocations: )];

	// --- destination ------------------------------------------------------
	NSTextField* destLabel = makeLabel( @"Install to" );

	self.destField                                        = [[NSTextField alloc] init];
	self.destField.translatesAutoresizingMaskIntoConstraints = NO;
	self.destField.placeholderString                         = @"Where the generated FFGL plugins are written";
	self.destField.stringValue = [[NSUserDefaults standardUserDefaults] stringForKey:kDefaultsDest] ?: @"";

	NSButton* chooseDest = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector( chooseDest: )];

	// --- quick destinations ----------------------------------------------
	NSStackView* quickRow  = [[NSStackView alloc] init];
	quickRow.orientation   = NSUserInterfaceLayoutOrientationHorizontal;
	quickRow.spacing       = 8;
	quickRow.translatesAutoresizingMaskIntoConstraints = NO;
	[quickRow addArrangedSubview:makeLabel( @"Add to" )];

	NSMutableArray< NSString* >* destinations = [NSMutableArray array];
	NSArray< NSString* >* products            = installedResolumeProducts();
	if( products.count == 0 )
		products = @[ @"Arena" ];// nothing detected; offer the common case anyway

	for( NSString* product in products )
	{
		NSButton* button = [NSButton buttonWithTitle:[NSString stringWithFormat:@"Resolume %@", product]
											  target:self
											  action:@selector( useQuickDestination: )];
		button.tag = (NSInteger)destinations.count;
		[destinations addObject:extraEffectsPath( product )];
		[quickRow addArrangedSubview:button];
	}
	self.quickDestinations = destinations;
	[quickRow addArrangedSubview:[NSView new]];// pushes the buttons left

	// --- action row -------------------------------------------------------
	self.startButton               = [NSButton buttonWithTitle:@"Start" target:self action:@selector( start: )];
	self.startButton.keyEquivalent = @"\r";

	self.cancelButton         = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector( cancel: )];
	self.cancelButton.enabled = NO;

	self.revealButton         = [NSButton buttonWithTitle:@"Reveal in Finder"
												   target:self
												   action:@selector( revealDestination: )];
	self.revealButton.enabled = NO;

	self.progress                                        = [[NSProgressIndicator alloc] init];
	self.progress.style                                  = NSProgressIndicatorStyleBar;
	self.progress.indeterminate                          = NO;
	self.progress.minValue                               = 0;
	self.progress.maxValue                               = 1;
	self.progress.doubleValue                            = 0;
	self.progress.translatesAutoresizingMaskIntoConstraints = NO;

	self.statusLabel           = makeLabel( @"Idle" );
	self.statusLabel.textColor = [NSColor secondaryLabelColor];

	NSStackView* actionRow = [[NSStackView alloc] init];
	actionRow.orientation  = NSUserInterfaceLayoutOrientationHorizontal;
	actionRow.spacing      = 8;
	actionRow.translatesAutoresizingMaskIntoConstraints = NO;
	[actionRow addArrangedSubview:self.startButton];
	[actionRow addArrangedSubview:self.cancelButton];
	[actionRow addArrangedSubview:self.revealButton];
	[actionRow addArrangedSubview:[NSView new]];

	// --- log --------------------------------------------------------------
	NSScrollView* scroll   = [[NSScrollView alloc] init];
	scroll.hasVerticalScroller                          = YES;
	scroll.borderType                                   = NSBezelBorder;
	scroll.translatesAutoresizingMaskIntoConstraints    = NO;

	self.logView                          = [[NSTextView alloc] initWithFrame:NSMakeRect( 0, 0, 760, 320 )];
	self.logView.minSize                  = NSMakeSize( 0, 0 );
	self.logView.maxSize                  = NSMakeSize( FLT_MAX, FLT_MAX );
	self.logView.verticallyResizable      = YES;
	self.logView.horizontallyResizable    = NO;
	self.logView.autoresizingMask         = NSViewWidthSizable;
	self.logView.editable                 = NO;
	self.logView.richText                 = YES;
	self.logView.drawsBackground          = YES;
	self.logView.backgroundColor          = [NSColor colorWithCalibratedRed:0.09 green:0.10 blue:0.11 alpha:1.0];
	self.logView.textContainerInset       = NSMakeSize( 6, 6 );
	self.logView.textContainer.widthTracksTextView = YES;
	scroll.documentView                   = self.logView;

	// --- layout -----------------------------------------------------------
	NSStackView* stack   = [[NSStackView alloc] init];
	stack.orientation    = NSUserInterfaceLayoutOrientationVertical;
	stack.alignment      = NSLayoutAttributeLeading;
	stack.spacing        = 10;
	stack.edgeInsets     = NSEdgeInsetsMake( 16, 16, 16, 16 );
	stack.translatesAutoresizingMaskIntoConstraints = NO;

	NSStackView* sourceRow = [NSStackView stackViewWithViews:@[ sourceLabel, self.sourceField, chooseSource, clearSource ]];
	sourceRow.spacing      = 8;
	NSStackView* destRow   = [NSStackView stackViewWithViews:@[ destLabel, self.destField, chooseDest ]];
	destRow.spacing        = 8;

	for( NSStackView* row in @[ sourceRow, destRow ] )
	{
		row.orientation                            = NSUserInterfaceLayoutOrientationHorizontal;
		row.translatesAutoresizingMaskIntoConstraints = NO;
		[stack addArrangedSubview:row];
		[row.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-32].active = YES;
	}

	[stack addArrangedSubview:quickRow];
	[stack addArrangedSubview:actionRow];
	[stack addArrangedSubview:self.progress];
	[stack addArrangedSubview:self.statusLabel];
	[stack addArrangedSubview:scroll];

	[content addSubview:stack];

	// Keep the two labels the same width so the fields line up.
	[sourceLabel.widthAnchor constraintEqualToConstant:78].active = YES;
	[destLabel.widthAnchor constraintEqualToConstant:78].active   = YES;

	[NSLayoutConstraint activateConstraints:@[
		[stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
		[stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
		[stack.topAnchor constraintEqualToAnchor:content.topAnchor],
		[stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
		[self.progress.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-32],
		[scroll.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-32],
		[scroll.heightAnchor constraintGreaterThanOrEqualToConstant:220],
	]];

	self.window.initialFirstResponder = self.startButton;

	[self appendLine:@"OFX Bridge — generates one FFGL plugin per OFX plugin." kind:LogKind::Heading];
	[self appendLine:@"Pick a folder (or a single .ofx.bundle), pick where to install, then press Start."
				kind:LogKind::Info];
}

#pragma mark - Actions

- ( void )chooseSource:( id )sender
{
	NSOpenPanel* panel           = [NSOpenPanel openPanel];
	panel.canChooseDirectories   = YES;
	panel.canChooseFiles         = YES;
	panel.allowsMultipleSelection = NO;
	panel.message                = @"Choose a folder of OFX plugins, or one .ofx.bundle";
	panel.prompt                 = @"Select";

	if( [panel runModal] == NSModalResponseOK )
		self.sourceField.stringValue = forDisplay( panel.URL.path );
}

- ( void )useDefaultLocations:( id )sender
{
	self.sourceField.stringValue = @"";

	[self appendLine:@"Default OFX locations:" kind:LogKind::Info];
	for( const std::string& p : ofxbridge::defaultSearchPaths() )
		[self appendLine:[NSString stringWithFormat:@"  %s", p.c_str()] kind:LogKind::Info];
}

- ( void )chooseDest:( id )sender
{
	NSOpenPanel* panel              = [NSOpenPanel openPanel];
	panel.canChooseDirectories      = YES;
	panel.canChooseFiles            = NO;
	panel.canCreateDirectories      = YES;
	panel.allowsMultipleSelection   = NO;
	panel.message                   = @"Choose where the generated FFGL plugins go";
	panel.prompt                    = @"Select";

	if( [panel runModal] == NSModalResponseOK )
		self.destField.stringValue = forDisplay( panel.URL.path );
}

- ( void )useQuickDestination:( NSButton* )sender
{
	if( sender.tag < 0 || (NSUInteger)sender.tag >= self.quickDestinations.count )
		return;

	NSString* path             = forDisplay( self.quickDestinations[ (NSUInteger)sender.tag ] );
	self.destField.stringValue = path;
	[self appendLine:[NSString stringWithFormat:@"Destination: %@", path] kind:LogKind::Info];

	NSString* product = [sender.title stringByReplacingOccurrencesOfString:@"Resolume " withString:@""];
	if( !productHostsEffects( product ) )
		[self appendLine:[NSString stringWithFormat:@"Note: only Arena and Avenue are confirmed to scan an "
													@"\"Extra Effects\" folder. %@ may ignore what is installed here.",
													sender.title]
					kind:LogKind::Warn];
}

- ( void )revealDestination:( id )sender
{
	NSString* dest = self.destField.stringValue;
	if( dest.length > 0 )
		[[NSWorkspace sharedWorkspace] selectFile:nil inFileViewerRootedAtPath:forUse( dest )];
}

- ( void )cancel:( id )sender
{
	if( _cancelFlag )
		_cancelFlag->store( true );
	self.cancelButton.enabled = NO;
	self.statusLabel.stringValue = @"Cancelling…";
}

- ( void )start:( id )sender
{
	NSString* source = [self.sourceField.stringValue
		stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	NSString* dest = [self.destField.stringValue
		stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];

	if( dest.length == 0 )
	{
		NSAlert* alert     = [[NSAlert alloc] init];
		alert.messageText  = @"Choose a destination";
		alert.informativeText = @"Pick where the generated FFGL plugins should be installed.";
		[alert runModal];
		return;
	}

	NSString* templatePath = [self locateTemplate];
	if( templatePath == nil )
	{
		[self appendLine:@"Cannot find ofxwrapper.bundle, the prebuilt plugin every generated bundle is a copy of. "
						 @"Build it (cmake --build build --target ofxwrapper) or reinstall the app."
					kind:LogKind::Bad];
		return;
	}

	[[NSUserDefaults standardUserDefaults] setObject:source forKey:kDefaultsSource];
	[[NSUserDefaults standardUserDefaults] setObject:dest forKey:kDefaultsDest];

	// The fields hold display paths, which may be `~`-relative; everything below
	// this line works in real ones.
	NSString* sourcePath = source.length > 0 ? forUse( source ) : @"";
	NSString* destPath   = forUse( dest );

	ofxgen::Options options;
	options.outDir       = destPath.UTF8String;
	options.templatePath = templatePath.UTF8String;

	if( sourcePath.length > 0 )
	{
		// A .ofx.bundle is a directory, so the open panel hands one back like any
		// other folder. Scanning it directly finds nothing (the OFX search path is
		// directory-scoped), so it becomes a filter instead.
		if( [sourcePath hasSuffix:@".ofx.bundle"] || [sourcePath hasSuffix:@".ofx"] )
			options.onlyBundlePath = sourcePath.UTF8String;
		else
			options.searchPaths = { std::string( sourcePath.UTF8String ) };
	}

	[self appendLine:@"" kind:LogKind::Info];
	[self appendLine:[NSString stringWithFormat:@"Generating into %@", dest] kind:LogKind::Heading];
	[self appendLine:[NSString stringWithFormat:@"Source: %@", source.length > 0 ? source : @"default OFX locations"]
				kind:LogKind::Info];

	self.startButton.enabled     = NO;
	self.cancelButton.enabled    = YES;
	self.revealButton.enabled    = NO;
	self.progress.indeterminate  = YES;
	[self.progress startAnimation:nil];
	self.statusLabel.stringValue = @"Scanning…";

	_cancelFlag                                  = std::make_shared< std::atomic< bool > >( false );
	std::shared_ptr< std::atomic< bool > > flag = _cancelFlag;

	// Captured strongly on purpose: the window must outlive the run, and nothing
	// on the window owns these callbacks, so there is no cycle to break.
	BridgeWindow* controller = self;

	dispatch_async( dispatch_get_global_queue( QOS_CLASS_USER_INITIATED, 0 ), ^{
		// Describing a plugin runs its code, so everything below is deliberately
		// off the main thread; the callbacks hop back for anything that touches
		// the UI.
		const ofxgen::Result result = ofxgen::generate(
			options,
			[controller]( const std::string& line ) {
				NSString* text = [NSString stringWithUTF8String:line.c_str()] ?: @"";
				dispatch_async( dispatch_get_main_queue(), ^{
					[controller appendLine:text kind:[BridgeWindow kindForLine:text]];
				} );
			},
			[controller]( int done, int total, const std::string& label ) {
				NSString* text = [NSString stringWithUTF8String:label.c_str()] ?: @"";
				dispatch_async( dispatch_get_main_queue(), ^{
					[controller setProgressDone:done total:total label:text];
				} );
			},
			[flag]() { return flag->load(); } );

		dispatch_async( dispatch_get_main_queue(), ^{
			[controller finishWithGenerated:result.generated
									skipped:result.skipped
								  cancelled:result.cancelled
									  error:result.error.empty()
												? nil
												: [NSString stringWithUTF8String:result.error.c_str()]];
		} );
	} );
}

#pragma mark - Worker callbacks (main thread)

- ( void )setProgressDone:( int )done total:( int )total label:( NSString* )label
{
	if( total <= 0 )
	{
		self.progress.indeterminate = YES;
		[self.progress startAnimation:nil];
		if( label.length > 0 )
			self.statusLabel.stringValue = label;
		return;
	}

	[self.progress stopAnimation:nil];
	self.progress.indeterminate = NO;
	self.progress.maxValue      = total;
	self.progress.doubleValue   = done;

	self.statusLabel.stringValue =
		label.length > 0 ? [NSString stringWithFormat:@"%d of %d — %@", done + 1, total, label]
						 : [NSString stringWithFormat:@"%d of %d", done, total];
}

- ( void )finishWithGenerated:( int )generated skipped:( int )skipped cancelled:( BOOL )cancelled error:( NSString* )error
{
	[self.progress stopAnimation:nil];
	self.progress.indeterminate = NO;
	self.startButton.enabled    = YES;
	self.cancelButton.enabled   = NO;
	_cancelFlag.reset();

	if( error != nil )
	{
		[self appendLine:error kind:LogKind::Bad];
		self.statusLabel.stringValue = @"Failed";
		self.progress.doubleValue    = 0;
		return;
	}

	NSString* summary = [NSString stringWithFormat:@"%d generated, %d skipped%@", generated, skipped,
													cancelled ? @" (cancelled)" : @""];
	[self appendLine:summary kind:generated > 0 ? LogKind::Good : LogKind::Warn];
	self.statusLabel.stringValue = summary;

	if( generated > 0 )
	{
		self.revealButton.enabled = YES;
		[self appendLine:@"Restart Resolume, or rescan its effects, to pick these up." kind:LogKind::Info];
	}
}

#pragma mark - Helpers

/// The prebuilt wrapper ships inside this app's Resources; the build-tree copy
/// beside the executable is the fallback for development.
- ( NSString* )locateTemplate
{
	NSString* bundled = [[NSBundle mainBundle] pathForResource:@"ofxwrapper" ofType:@"bundle"];
	if( bundled != nil )
		return bundled;

	const std::string found = ofxgen::findTemplate( [[NSBundle mainBundle] executablePath].UTF8String );
	return found.empty() ? nil : [NSString stringWithUTF8String:found.c_str()];
}

+ ( LogKind )kindForLine:( NSString* )line
{
	if( [line hasPrefix:@"  built"] )
		return LogKind::Good;
	if( [line hasPrefix:@"  skip"] || [line containsString:@"FAIL"] )
		return LogKind::Warn;
	if( [line hasPrefix:@"  fail"] )
		return LogKind::Bad;
	if( [line hasPrefix:@"search path:"] || [line hasPrefix:@"found "] )
		return LogKind::Heading;
	return LogKind::Info;
}

- ( void )appendLine:( NSString* )line kind:( LogKind )kind
{
	NSColor* colour = nil;
	switch( kind )
	{
	case LogKind::Heading: colour = [NSColor colorWithCalibratedRed:0.45 green:0.78 blue:0.98 alpha:1.0]; break;
	case LogKind::Good: colour = [NSColor colorWithCalibratedRed:0.49 green:0.87 blue:0.51 alpha:1.0]; break;
	case LogKind::Warn: colour = [NSColor colorWithCalibratedRed:0.96 green:0.76 blue:0.35 alpha:1.0]; break;
	case LogKind::Bad: colour = [NSColor colorWithCalibratedRed:0.95 green:0.45 blue:0.42 alpha:1.0]; break;
	case LogKind::Info:
	default: colour = [NSColor colorWithCalibratedWhite:0.82 alpha:1.0]; break;
	}

	NSAttributedString* text =
		[[NSAttributedString alloc] initWithString:[line stringByAppendingString:@"\n"]
										attributes:@{ NSFontAttributeName : self.logFont,
													  NSForegroundColorAttributeName : colour }];
	[self.logView.textStorage appendAttributedString:text];
	[self.logView scrollRangeToVisible:NSMakeRange( self.logView.string.length, 0 )];
}

#pragma mark - NSWindowDelegate

- ( void )windowWillClose:( NSNotification* )notification
{
	if( _cancelFlag )
		_cancelFlag->store( true );
	[NSApp terminate:nil];
}

@end
