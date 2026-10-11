// Copyright 2026 Tom Rothamel. See LICENSE.txt.
// This translation unit uses Cocoa's reference counting (no ARC required).
#include "nativefiledialog.h"
#import <Cocoa/Cocoa.h>

static NSString *string(const std::string &value) {
    return [[[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding] autorelease];
}

void *nfd_mac_retain_parent(void *window) {
    return [static_cast<NSWindow *>(window) retain];
}

void nfd_mac_release_parent(NFDRequest *r) {
    [static_cast<NSWindow *>(r->window) release];
    r->window = nullptr;
}

void nfd_mac_start(NFDRequest *r) {
    r->backend = "macos-cocoa";
    if (![NSThread isMainThread]) {
        nfd_finish(r, 4, "", "Cocoa file dialogs require the main thread.");
        return;
    }
    if (r->cancelled) { nfd_finish(r, 2, "", ""); return; }
    @try {
        NSSavePanel *panel;
        if (r->kind == "save") {
            panel = [NSSavePanel savePanel];
        } else {
            NSOpenPanel *open = [NSOpenPanel openPanel];
            [open setAllowsMultipleSelection:NO];
            [open setCanChooseFiles:r->kind == "open"];
            [open setCanChooseDirectories:r->kind == "folder"];
            panel = open;
        }
        NSString *title = string(r->title), *initial = string(r->initial);
        if (!title || !initial) {
            nfd_finish(r, 4, "", "Cocoa could not decode a file dialog argument as UTF-8.");
            return;
        }
        [panel setTitle:title];
        [panel setCanCreateDirectories:YES];
        [panel setExtensionHidden:NO];
        if ([initial length]) {
            NSString *directory = initial;
            if (r->kind != "folder") {
                if (![initial hasSuffix:@"/"]) [panel setNameFieldStringValue:[initial lastPathComponent]];
                directory = [initial stringByDeletingLastPathComponent];
            }
            [panel setDirectoryURL:[NSURL fileURLWithPath:directory isDirectory:YES]];
        }
        if (r->kind != "folder" && !r->filters.empty()) {
            NSMutableArray *extensions = [NSMutableArray array];
            bool all = false;
            for (auto &filter : r->filters) {
                for (auto &pattern : filter.patterns) {
                    if (pattern == "*" || pattern == "*.*") { all = true; break; }
                    if (pattern.size() > 2 && pattern.substr(0, 2) == "*.") {
                        NSString *extension = string(pattern.substr(2));
                        if (extension && ![extensions containsObject:extension]) [extensions addObject:extension];
                    }
                }
            }
            if (!all && [extensions count]) [panel setAllowedFileTypes:extensions];
            [panel setAllowsOtherFileTypes:all];
        }
        r->panel = [panel retain];
        nfd_retain(r); // The completion block may outlive the Python handle.
        void (^completion)(NSModalResponse) = ^(NSModalResponse response) {
            @try {
                if (r->cancelled || response == NSModalResponseCancel) {
                    nfd_finish(r, 2, "", "");
                } else if (response == NSModalResponseOK) {
                    NSURL *url = [panel URL];
                    const char *path = [[url path] fileSystemRepresentation];
                    if ([url isFileURL] && path) nfd_finish(r, 1, path, "");
                    else nfd_finish(r, 4, "", "Cocoa returned an invalid file URL.");
                } else {
                    nfd_finish(r, 4, "", "Cocoa returned an unexpected modal response.");
                }
            } @catch (NSException *exception) {
                nfd_finish(r, 4, "", [[exception reason] UTF8String]);
            }
            [static_cast<NSSavePanel *>(r->panel) release];
            r->panel = nullptr;
            nfd_release(r);
        };
        NSWindow *parent = static_cast<NSWindow *>(r->window);
        r->parent_attached = parent != nil;
        if (parent) [panel beginSheetModalForWindow:parent completionHandler:completion];
        else [panel beginWithCompletionHandler:completion];
    } @catch (NSException *exception) {
        if (r->panel) {
            [static_cast<NSSavePanel *>(r->panel) release];
            r->panel = nullptr;
            nfd_release(r);
        }
        nfd_finish(r, 4, "", [[exception reason] UTF8String]);
    }
}

void nfd_mac_cancel(NFDRequest *r) {
    if ([NSThread isMainThread] && r->panel) [static_cast<NSSavePanel *>(r->panel) cancel:nil];
}
