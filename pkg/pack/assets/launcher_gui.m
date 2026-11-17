// SPDX-License-Identifier: GPL-2.0-or-later
// NPS5 Launcher: lists the titles built with `nps5 build` (one title.json per
// ~/Library/Application Support/nativeps5/<titleId>/) with their icon and a Play button.
#import <Cocoa/Cocoa.h>

@interface AppDel : NSObject <NSApplicationDelegate>
@property(strong) NSWindow *win;
@property(strong) NSStackView *list;
@end

@implementation AppDel
- (NSString *)root {
  const char *h = getenv("NPS5_HOME");
  if (h && *h) return @(h);
  return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/nativeps5"];
}
- (void)play:(NSButton *)b {
  NSURL *u = [NSURL fileURLWithPath:b.identifier];
  NSWorkspaceOpenConfiguration *c = [NSWorkspaceOpenConfiguration configuration];
  c.createsNewApplicationInstance = YES;
  [[NSWorkspace sharedWorkspace] openApplicationAtURL:u configuration:c completionHandler:nil];
}
- (void)reload:(id)sender {
  for (NSView *v in [self.list.arrangedSubviews copy]) { [self.list removeArrangedSubview:v]; [v removeFromSuperview]; }
  NSFileManager *fm = [NSFileManager defaultManager];
  NSArray *ids = [[fm contentsOfDirectoryAtPath:[self root] error:nil] sortedArrayUsingSelector:@selector(compare:)];
  int n = 0;
  for (NSString *tid in ids) {
    NSData *d = [NSData dataWithContentsOfFile:[[[self root] stringByAppendingPathComponent:tid] stringByAppendingPathComponent:@"title.json"]];
    NSDictionary *t = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    NSString *app = t[@"app"];
    if (![app isKindOfClass:[NSString class]] || ![fm fileExistsAtPath:app]) continue;
    NSImageView *icon = [NSImageView imageViewWithImage:[[NSWorkspace sharedWorkspace] iconForFile:app]];
    [icon.widthAnchor constraintEqualToConstant:48].active = YES;
    [icon.heightAnchor constraintEqualToConstant:48].active = YES;
    NSTextField *name = [NSTextField labelWithString:[NSString stringWithFormat:@"%@  (%@, %@)", t[@"name"], tid, t[@"version"]]];
    name.font = [NSFont systemFontOfSize:14 weight:NSFontWeightMedium];
    [name setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSButton *play = [NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];
    play.identifier = app;
    NSStackView *row = [NSStackView stackViewWithViews:@[icon, name, play]];
    row.spacing = 12;
    row.alignment = NSLayoutAttributeCenterY;
    [self.list addArrangedSubview:row];
    [row.widthAnchor constraintEqualToAnchor:self.list.widthAnchor].active = YES;
    n++;
  }
  if (!n) [self.list addArrangedSubview:[NSTextField labelWithString:@"No titles built yet. Run: nps5 build <title dir>"]];
}
- (void)applicationDidFinishLaunching:(NSNotification *)note {
  self.win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 420)
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                           backing:NSBackingStoreBuffered defer:NO];
  self.win.title = @"NPS5 Games";
  self.list = [[NSStackView alloc] init];
  self.list.orientation = NSUserInterfaceLayoutOrientationVertical;
  self.list.alignment = NSLayoutAttributeLeading;
  self.list.spacing = 10;
  self.list.edgeInsets = NSEdgeInsetsMake(14, 14, 14, 14);
  NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 520, 420)];
  sv.hasVerticalScroller = YES;
  sv.documentView = self.list;
  self.list.translatesAutoresizingMaskIntoConstraints = NO;
  [self.list.widthAnchor constraintEqualToAnchor:sv.contentView.widthAnchor].active = YES;
  [self.list.topAnchor constraintEqualToAnchor:sv.contentView.topAnchor].active = YES;
  self.win.contentView = sv;
  [self reload:nil];
  [self.win center];
  [self.win makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }
- (void)applicationDidBecomeActive:(NSNotification *)n { if (self.list) [self reload:nil]; }
@end

int main(void) {
  @autoreleasepool {
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    AppDel *d = [AppDel new];
    app.delegate = d;
    [app run];
  }
  return 0;
}
