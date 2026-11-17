// The iOS app icon from the Mac game's icon (AC2.icns, game art: it is read
// from the local copy at build time, never committed). iOS wants an opaque
// square, so the largest image in the .icns is drawn over a dark background.
//   swift make_icon.swift <in.icns> <out.png>
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count == 3 else {
    FileHandle.standardError.write("usage: make_icon <in.icns> <out.png>\n".data(using: .utf8)!)
    exit(2)
}
guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil) else {
    FileHandle.standardError.write("make_icon: cannot read \(args[1])\n".data(using: .utf8)!)
    exit(1)
}
var best: CGImage?
for i in 0..<CGImageSourceGetCount(source) {
    if let image = CGImageSourceCreateImageAtIndex(source, i, nil), image.width > (best?.width ?? 0) {
        best = image
    }
}
guard let icon = best else {
    FileHandle.standardError.write("make_icon: no image in \(args[1])\n".data(using: .utf8)!)
    exit(1)
}

let size = 1024
let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
let full = CGRect(x: 0, y: 0, width: size, height: size)
let gradient = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
                          colors: [CGColor(srgbRed: 0.20, green: 0.20, blue: 0.22, alpha: 1),
                                   CGColor(srgbRed: 0.05, green: 0.05, blue: 0.06, alpha: 1)] as CFArray,
                          locations: [0, 1])!
context.drawLinearGradient(gradient, start: CGPoint(x: 0, y: CGFloat(size)), end: .zero, options: [])
context.interpolationQuality = .high
// A .icns is the Mac game's icon: drawn with a margin over the background. A .png is taken as a finished
// icon: drawn edge to edge (the background only shows through its transparent pixels).
let isPNG = URL(fileURLWithPath: args[1]).pathExtension.lowercased() == "png"
let inset: CGFloat = isPNG ? 0 : 0.08
context.draw(icon, in: full.insetBy(dx: CGFloat(size) * inset, dy: CGFloat(size) * inset))

guard let out = context.makeImage(),
      let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: args[2]) as CFURL,
                                                 UTType.png.identifier as CFString, 1, nil) else {
    exit(1)
}
CGImageDestinationAddImage(dest, out, nil)
exit(CGImageDestinationFinalize(dest) ? 0 : 1)
