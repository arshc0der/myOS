# Cameras (v2.10.0)
MyOS has a USB video class (UVC) driver: src/drivers/usb/uvc.c, with isochronous transfers added to xhci.c.
Most laptop built-in cameras are UVC devices on an internal USB port, so they use the same driver.

How it works: reads formats (YUY2, MJPEG) -> picks the size nearest 320x240 -> VS_PROBE/VS_COMMIT ->
picks the alternate setting with enough bandwidth -> starts an isochronous stream -> assembles frames.
The Camera app lists it as "USB camera vid:pid (WxH format)" and shows it at 320x240.

Limits (honest):
- Only xHCI (USB 3) host controllers. EHCI/OHCI/UHCI have no isochronous support yet (the log says so).
- USB 2 cameras (the usual kind, also behind xHCI) only; SuperSpeed cameras are skipped.
- No brightness/focus/zoom controls, no sound, camera streams from plug-in. MJPEG cameras need the stream to
  contain Huffman tables (most do).
- Cameras that are not UVC (some Apple/MIPI/IR cameras) are not supported.
- NOT TESTED on real hardware. If it does not work, run `lsusb` and send the log lines starting with "usb".
