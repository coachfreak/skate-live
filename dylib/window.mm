// Branding for the game started by Skate.live: window title "Skate.live [Host]" or
// "Skate.live [Client]", Dock name "Skate.live" and the launcher's icon. The runtime
// titles its SDL window "skate3recomp <version>" once, when it creates it; the
// launcher passes the role in SKATE3_LAN_ROLE and its icon in SKATE3_LAN_ICON.

#import <AppKit/AppKit.h>

#include <dlfcn.h>

#include <cstdlib>

namespace {

// The Dock and Cmd-Tab name: LaunchServices' display-name item for this running
// app (private API, looked up so a missing symbol just skips it).
void RenameApp(NSString* name) {
  using GetASN = CFTypeRef (*)();
  using SetItem = OSStatus (*)(int, CFTypeRef, CFStringRef, CFTypeRef, CFDictionaryRef*);
  const auto get_asn = reinterpret_cast<GetASN>(dlsym(RTLD_DEFAULT, "_LSGetCurrentApplicationASN"));
  const auto set_item = reinterpret_cast<SetItem>(dlsym(RTLD_DEFAULT, "_LSSetApplicationInformationItem"));
  const auto key = static_cast<const CFStringRef*>(dlsym(RTLD_DEFAULT, "_kLSDisplayNameKey"));
  const CFTypeRef asn = get_asn ? get_asn() : nullptr;
  if (set_item && key && asn) set_item(-2 /* kLSDefaultSessionID */, asn, *key, (__bridge CFStringRef)name, nullptr);
}

__attribute__((constructor)) void BrandGame() {
  const char* role = std::getenv("SKATE3_LAN_ROLE");
  const char* icon_path = std::getenv("SKATE3_LAN_ICON");
  NSString* title = role ? [NSString stringWithFormat:@"Skate.live [%s]", role] : @"Skate.live";
  // Runs on the main thread once the game's event loop does (SDL pumps it).
  dispatch_async(dispatch_get_main_queue(), ^{
    NSImage* icon = icon_path ? [[NSImage alloc] initWithContentsOfFile:@(icon_path)] : nil;
    __block bool app_branded = false;
    // ponytail: polls twice a second for the life of the game; a no-op once renamed.
    [NSTimer scheduledTimerWithTimeInterval:0.5
                                    repeats:YES
                                      block:^(NSTimer*) {
                                        if (!NSApp) return;
                                        if (!app_branded) {
                                          if (icon) NSApp.applicationIconImage = icon;
                                          RenameApp(@"Skate.live");
                                          app_branded = true;
                                        }
                                        for (NSWindow* window in NSApp.windows) {
                                          if ([window.title hasPrefix:@"skate3recomp"]) window.title = title;
                                        }
                                      }];
  });
}

}  // namespace
