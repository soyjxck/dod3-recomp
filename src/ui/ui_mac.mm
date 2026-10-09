/*
 * The installer's window on macOS (src/ui/ui.h): a window with a Metal
 * view, Dear ImGui's OSX and Metal backends, and the open panels. The wizard
 * (src/ui/wizard.cpp) draws the pages; this file only gives it frames.
 *
 * ImGui works in points here: io.DisplaySize is the window's size in points,
 * io.DisplayFramebufferScale is the Retina factor (the OSX backend sets it
 * each frame), and 1.92 rasterises the font at that scale, so text is crisp
 * without scaling the font by hand. Pads reach ImGui through the
 * GameController framework (the OSX backend).
 *
 * DOD3_SETUP_GRAB=<prefix>: while <prefix>.req exists, the next frame is
 * written to <prefix>.ppm and the request removed -- the game's
 * PS3RECOMP_METAL_FRAME_GRAB protocol, for checking the pages without
 * screenshots.
 */
#include "ui/ui.h"

#include "imgui.h"
#include "backends/imgui_impl_metal.h"
#include "backends/imgui_impl_osx.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

/* Closing the window (its close button, or Quit in the menu) is the wizard's
 * Quit: the window stays until ui_shutdown, so the frame in flight is safe. */
@interface Dod3SetupWindowDelegate : NSObject <NSWindowDelegate>
@property(atomic) BOOL closed;
- (void)quit:(id)sender;
@end
@implementation Dod3SetupWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    (void)sender;
    self.closed = YES;
    return NO;
}
- (void)quit:(id)sender
{
    (void)sender;
    self.closed = YES;
}
@end

namespace dod3setup {
namespace {

struct Ui {
    NSWindow* window;
    MTKView* view;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    Dod3SetupWindowDelegate* delegate;
    NSMenu* menu_before;
    MTLRenderPassDescriptor* pass;      /* this frame's, with its drawable */
    id<CAMetalDrawable> drawable;
    MTLRenderPassDescriptor* hidden_pass;   /* for frames with the window out of sight */
    bool live;
};
Ui s_ui;

/* The window is out of sight (minimised, covered, another Space): build the
 * frame against a 1x1 target and draw nothing, rather than wait on drawables. */
bool window_visible()
{
    return s_ui.window.isVisible && !s_ui.window.isMiniaturized &&
           (s_ui.window.occlusionState & NSWindowOcclusionStateVisible);
}

MTLRenderPassDescriptor* hidden_pass()
{
    if (!s_ui.hidden_pass) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s_ui.view.colorPixelFormat
                                                                                      width:1
                                                                                     height:1
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        s_ui.hidden_pass = [MTLRenderPassDescriptor renderPassDescriptor];
        s_ui.hidden_pass.colorAttachments[0].texture = [s_ui.device newTextureWithDescriptor:td];
        s_ui.hidden_pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        s_ui.hidden_pass.colorAttachments[0].storeAction = MTLStoreActionDontCare;
    }
    return s_ui.hidden_pass;
}

/* A minimal menu bar while the installer runs, so Cmd-Q quits it; the
 * game's own (rsx_metal_backend.m) is made when its window is. */
void install_menu()
{
    s_ui.menu_before = NSApp.mainMenu;
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* app = [[NSMenuItem alloc] init];
    NSMenu* m = [[NSMenu alloc] init];
    NSMenuItem* q = [[NSMenuItem alloc] initWithTitle:@"Quit Setup" action:@selector(quit:) keyEquivalent:@"q"];
    q.target = s_ui.delegate;
    [m addItem:q];
    app.submenu = m;
    [bar addItem:app];
    NSApp.mainMenu = bar;
}

void pump_events()
{
    for (;;) {
        NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate distantPast]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
        if (!e) break;
        [NSApp sendEvent:e];
    }
}

/* DOD3_SETUP_GRAB: the drawable's pixels as a PPM. */
void grab_if_asked(id<MTLCommandBuffer> cb, id<MTLTexture> tex)
{
    static const char* prefix = getenv("DOD3_SETUP_GRAB");
    if (!prefix || !*prefix) return;
    std::string req = std::string(prefix) + ".req", out = std::string(prefix) + ".ppm";
    struct stat st;
    if (stat(req.c_str(), &st) != 0) return;
    unlink(req.c_str());
    const NSUInteger w = tex.width, h = tex.height;
    id<MTLBuffer> buf = [s_ui.device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:w * 4
        destinationBytesPerImage:w * h * 4];
    [blit endEncoding];
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
      FILE* f = fopen((out + ".tmp").c_str(), "wb");
      if (!f) return;
      fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)w, (unsigned long)h);
      const uint8_t* p = (const uint8_t*)buf.contents;   /* BGRA */
      std::vector<uint8_t> row(w * 3);
      for (NSUInteger y = 0; y < h; y++) {
          for (NSUInteger x = 0; x < w; x++) {
              row[x * 3] = p[(y * w + x) * 4 + 2];
              row[x * 3 + 1] = p[(y * w + x) * 4 + 1];
              row[x * 3 + 2] = p[(y * w + x) * 4];
          }
          fwrite(row.data(), 1, row.size(), f);
      }
      fclose(f);
      rename((out + ".tmp").c_str(), out.c_str());
    }];
}

/* After a modal panel: ImGui saw the button go down on the window, not up. */
void after_panel()
{
    ImGuiIO& io = ImGui::GetIO();
    for (int b = 0; b < ImGuiMouseButton_COUNT; b++) io.AddMouseButtonEvent(b, false);
    io.ClearInputKeys();
    [s_ui.window makeKeyAndOrderFront:nil];
}

}  // namespace

bool ui_init(const char* title, int width, int height, const std::string& font_path, float font_px)
{
    @autoreleasepool {
        s_ui.device = MTLCreateSystemDefaultDevice();
        if (!s_ui.device) return false;
        s_ui.queue = [s_ui.device newCommandQueue];

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        static bool launched = false;
        if (!launched) {
            [NSApp finishLaunching];
            launched = true;
        }

        s_ui.delegate = [[Dod3SetupWindowDelegate alloc] init];
        s_ui.window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, width, height)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        s_ui.window.title = [NSString stringWithUTF8String:title ? title : ""];
        s_ui.window.releasedWhenClosed = NO;
        s_ui.window.delegate = s_ui.delegate;
        s_ui.view = [[MTKView alloc] initWithFrame:NSMakeRect(0, 0, width, height) device:s_ui.device];
        s_ui.view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        s_ui.view.clearColor = MTLClearColorMake(0, 0, 0, 1);
        s_ui.view.paused = YES;                  /* frames come from ui_frame_end, not a timer */
        s_ui.view.enableSetNeedsDisplay = NO;
        s_ui.view.framebufferOnly = getenv("DOD3_SETUP_GRAB") ? NO : YES;
        s_ui.window.contentView = s_ui.view;
        [s_ui.window center];
        install_menu();
        [s_ui.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;                /* no imgui.ini in the data folder */
        if (!ImGui_ImplOSX_Init(s_ui.view) || !ImGui_ImplMetal_Init(s_ui.device)) {
            ImGui::DestroyContext();
            [s_ui.window orderOut:nil];
            s_ui = Ui{};
            return false;
        }
        ImFont* font = nullptr;
        if (!font_path.empty()) font = io.Fonts->AddFontFromFileTTF(font_path.c_str(), font_px);
        if (!font) {
            ImFontConfig cfg;
            cfg.SizePixels = font_px;
            io.Fonts->AddFontDefault(&cfg);
        }
        s_ui.live = true;
        pump_events();
        return true;
    }
}

bool ui_frame_begin()
{
    @autoreleasepool {
        pump_events();
        if (!s_ui.live || s_ui.delegate.closed) return false;
        s_ui.pass = nil;
        s_ui.drawable = nil;
        if (window_visible()) {
            s_ui.pass = s_ui.view.currentRenderPassDescriptor;   /* waits for a drawable: the frame pacing */
            s_ui.drawable = s_ui.view.currentDrawable;
        }
        if (!s_ui.pass || !s_ui.drawable) {
            s_ui.pass = nil;
            s_ui.drawable = nil;
            usleep(16 * 1000);
        }
        ImGui_ImplMetal_NewFrame(s_ui.pass ? s_ui.pass : hidden_pass());
        ImGui_ImplOSX_NewFrame(s_ui.view);
        ImGui::NewFrame();
        return true;
    }
}

void ui_frame_end()
{
    @autoreleasepool {
        ImGui::Render();
        id<MTLCommandBuffer> cb = [s_ui.queue commandBuffer];
        MTLRenderPassDescriptor* pass = s_ui.pass ? s_ui.pass : hidden_pass();
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        /* With nothing to show it still runs: the backend uploads the
         * font atlas the frame asked for. */
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
        [enc endEncoding];
        if (s_ui.drawable) {
            grab_if_asked(cb, s_ui.drawable.texture);
            [cb presentDrawable:s_ui.drawable];
        }
        [cb commit];
        s_ui.pass = nil;
        s_ui.drawable = nil;
    }
}

void ui_shutdown()
{
    @autoreleasepool {
        if (!s_ui.live) return;
        id<MTLCommandBuffer> last = [s_ui.queue commandBuffer];   /* the GPU is done with ImGui's buffers */
        [last commit];
        [last waitUntilCompleted];
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplOSX_Shutdown();
        ImGui::DestroyContext();
        s_ui.window.delegate = nil;
        [s_ui.window orderOut:nil];
        [s_ui.window close];
        NSApp.mainMenu = s_ui.menu_before;
        pump_events();   /* the window goes now, not when the game's loop next runs */
        s_ui = Ui{};
    }
}

std::vector<std::filesystem::path> ui_pick_files(const char* title,
                                                 const std::vector<std::pair<std::string, std::string>>& filters)
{
    std::vector<std::filesystem::path> out;
    @autoreleasepool {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.message = [NSString stringWithUTF8String:title ? title : ""];
        p.canChooseFiles = YES;
        p.canChooseDirectories = NO;
        p.allowsMultipleSelection = YES;
        /* {description, "*.pkg;*.iso"}: every filter's extensions together
         * (the panel has no filter menu); "*.*" or "*" means anything. */
        NSMutableArray<UTType*>* types = [NSMutableArray array];
        bool any = filters.empty();
        for (const auto& f : filters) {
            size_t i = 0;
            const std::string& pat = f.second;
            while (i <= pat.size()) {
                size_t j = pat.find(';', i);
                if (j == std::string::npos) j = pat.size();
                std::string e = pat.substr(i, j - i);
                i = j + 1;
                while (!e.empty() && e.front() == ' ') e.erase(0, 1);
                if (e.rfind("*.", 0) == 0) e = e.substr(2);
                if (e.empty()) continue;
                if (e == "*") {
                    any = true;
                    continue;
                }
                if (UTType* t = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:e.c_str()]])
                    [types addObject:t];
            }
        }
        if (!any && types.count) p.allowedContentTypes = types;
        [NSApp activateIgnoringOtherApps:YES];
        if ([p runModal] == NSModalResponseOK)
            for (NSURL* u in p.URLs)
                if (u.fileSystemRepresentation) out.emplace_back(u.fileSystemRepresentation);
        if (s_ui.live) after_panel();
    }
    return out;
}

std::filesystem::path ui_pick_folder(const char* title)
{
    std::filesystem::path out;
    @autoreleasepool {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.message = [NSString stringWithUTF8String:title ? title : ""];
        p.canChooseFiles = NO;
        p.canChooseDirectories = YES;
        p.allowsMultipleSelection = NO;
        p.canCreateDirectories = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([p runModal] == NSModalResponseOK && p.URL.fileSystemRepresentation) out = p.URL.fileSystemRepresentation;
        if (s_ui.live) after_panel();
    }
    return out;
}

/* The system's UI font where ImGui can read it: San Francisco (a variable
 * TrueType; stb_truetype draws its default, regular instance), else
 * Helvetica Neue's collection, else Arial. */
std::string ui_default_font()
{
    static const char* const kFonts[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/HelveticaNeue.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    struct stat st;
    for (const char* f : kFonts)
        if (stat(f, &st) == 0) return f;
    return std::string();
}

}  // namespace dod3setup
