#!/usr/bin/env python3
"""
Enhanced OCR capture for Analogue Pocket terminal output.
Includes image preprocessing for better accuracy on terminal/console text.

Usage:
    ./capture_ocr_enhanced.py              # Single capture
    ./capture_ocr_enhanced.py --watch      # Continuous monitoring
    ./capture_ocr_enhanced.py --diff       # Only show new lines
"""

import subprocess
import sys
import os
import argparse
import time
import tempfile

def capture_frame(device="/dev/video0"):
    """Capture a single frame from video device."""
    with tempfile.NamedTemporaryFile(suffix='.png', delete=False) as f:
        screenshot_path = f.name

    cmd = [
        'ffmpeg', '-f', 'v4l2', '-i', device,
        '-frames:v', '1', '-q:v', '2',
        screenshot_path, '-y'
    ]

    result = subprocess.run(cmd, capture_output=True)
    if result.returncode != 0:
        print(f"ERROR: ffmpeg failed", file=sys.stderr)
        return None

    return screenshot_path

def preprocess_image(input_path):
    """Preprocess image for better OCR on terminal text."""
    try:
        from PIL import Image, ImageEnhance, ImageFilter, ImageOps
    except ImportError:
        print("WARNING: Pillow not installed, skipping preprocessing", file=sys.stderr)
        return input_path

    output_path = input_path.replace('.png', '_processed.png')

    img = Image.open(input_path)

    # Convert to grayscale
    img = img.convert('L')

    # Increase contrast (terminal text is usually high contrast)
    enhancer = ImageEnhance.Contrast(img)
    img = enhancer.enhance(2.0)

    # Sharpen
    img = img.filter(ImageFilter.SHARPEN)

    # Threshold to pure black/white (good for terminal text)
    img = img.point(lambda x: 0 if x < 128 else 255, '1')

    # Invert if needed (OCR works better with black text on white)
    # Check if background is dark (terminal usually has dark background)
    pixels = list(img.getdata())
    avg = sum(p for p in pixels) / len(pixels)
    if avg < 128:
        img = ImageOps.invert(img.convert('L'))

    # Scale up for better OCR (2x)
    img = img.resize((img.width * 2, img.height * 2), Image.LANCZOS)

    img.save(output_path)
    return output_path

def ocr_tesseract(image_path):
    """Run Tesseract OCR with terminal-optimized settings."""
    cmd = [
        'tesseract', image_path, 'stdout',
        '-l', 'eng',
        '--psm', '6',      # Assume uniform block of text
        '--oem', '3',      # Default OCR engine mode
        '-c', 'tessedit_char_whitelist=ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,;:!?()-_=+[]{}/<>\'\"@#$%^&*~`\\|',
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.stdout

def ocr_easyocr(image_path):
    """Run EasyOCR (better for varied fonts but slower)."""
    try:
        import easyocr
        reader = easyocr.Reader(['en'], gpu=False, verbose=False)
        result = reader.readtext(image_path, detail=0, paragraph=True)
        return '\n'.join(result)
    except ImportError:
        return None

def do_ocr(image_path):
    """Try available OCR engines."""
    # Try tesseract first (faster)
    if subprocess.run(['which', 'tesseract'], capture_output=True).returncode == 0:
        return ocr_tesseract(image_path)

    # Try EasyOCR
    result = ocr_easyocr(image_path)
    if result is not None:
        return result

    print("ERROR: No OCR engine available", file=sys.stderr)
    print("Install: sudo pacman -S tesseract tesseract-data-eng", file=sys.stderr)
    return ""

def capture_and_ocr(device="/dev/video0", preprocess=True):
    """Capture frame and run OCR."""
    screenshot = capture_frame(device)
    if not screenshot:
        return ""

    try:
        if preprocess:
            processed = preprocess_image(screenshot)
            text = do_ocr(processed)
            if processed != screenshot:
                os.unlink(processed)
        else:
            text = do_ocr(screenshot)
    finally:
        os.unlink(screenshot)

    return text

def watch_mode(device, interval=2.0, diff_only=False):
    """Continuously monitor and OCR the output."""
    print(f"Watching {device} (Ctrl+C to stop)...", file=sys.stderr)
    print("-" * 60, file=sys.stderr)

    last_text = ""

    try:
        while True:
            text = capture_and_ocr(device)

            if diff_only:
                # Only show new lines
                lines = text.split('\n')
                last_lines = last_text.split('\n')
                new_lines = [l for l in lines if l and l not in last_lines]
                if new_lines:
                    for line in new_lines:
                        print(line)
                    sys.stdout.flush()
            else:
                if text != last_text:
                    # Clear and print new text
                    print("\033[2J\033[H", end="")  # Clear screen
                    print(text)
                    sys.stdout.flush()

            last_text = text
            time.sleep(interval)

    except KeyboardInterrupt:
        print("\nStopped.", file=sys.stderr)

def main():
    parser = argparse.ArgumentParser(description='Capture and OCR Analogue Pocket output')
    parser.add_argument('--device', '-d', default='/dev/video0', help='Video device')
    parser.add_argument('--watch', '-w', action='store_true', help='Continuous monitoring')
    parser.add_argument('--diff', action='store_true', help='Only show new lines (with --watch)')
    parser.add_argument('--interval', '-i', type=float, default=2.0, help='Watch interval in seconds')
    parser.add_argument('--no-preprocess', action='store_true', help='Skip image preprocessing')
    parser.add_argument('--save', '-s', help='Save screenshot to file')

    args = parser.parse_args()

    if args.watch:
        watch_mode(args.device, args.interval, args.diff)
    else:
        if args.save:
            screenshot = capture_frame(args.device)
            if screenshot:
                import shutil
                shutil.copy(screenshot, args.save)
                print(f"Saved to {args.save}", file=sys.stderr)
                text = do_ocr(preprocess_image(screenshot) if not args.no_preprocess else screenshot)
                os.unlink(screenshot)
                print(text)
        else:
            text = capture_and_ocr(args.device, preprocess=not args.no_preprocess)
            print(text)

if __name__ == '__main__':
    main()
