import os
import math
from PIL import Image, ImageDraw, ImageFont

def rgb_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def export_image_to_c(img, var_name, with_alpha=True):
    w, h = img.size
    pixels = img.load()
    out = []
    out.append(f"// {var_name}: {w}x{h}\n")
    out.append(f"static const uint8_t {var_name}_map[] = {{\n")
    
    bytes_per_px = 3 if with_alpha else 2
    row_bytes = []
    for y in range(h):
        line = "    "
        for x in range(w):
            r, g, b, a = pixels[x, y]
            c565 = rgb_to_rgb565(r, g, b)
            low = c565 & 0xFF
            high = (c565 >> 8) & 0xFF
            if with_alpha:
                line += f"0x{low:02X}, 0x{high:02X}, 0x{a:02X}, "
            else:
                line += f"0x{low:02X}, 0x{high:02X}, "
        out.append(line + "\n")
        
    out.append("};\n\n")
    total_size = w * h * bytes_per_px
    cf_str = "LV_IMG_CF_TRUE_COLOR_ALPHA" if with_alpha else "LV_IMG_CF_TRUE_COLOR"
    out.append(f"const lv_img_dsc_t {var_name} = {{\n")
    out.append("    .header = {\n")
    out.append(f"        .cf = {cf_str},\n")
    out.append("        .always_zero = 0,\n")
    out.append("        .reserved = 0,\n")
    out.append(f"        .w = {w},\n")
    out.append(f"        .h = {h},\n")
    out.append("    },\n")
    out.append(f"    .data_size = {total_size},\n")
    out.append(f"    .data = {var_name}_map,\n")
    out.append("};\n\n")
    return "".join(out)

# 1. Snowflake
def make_snowflake(size=24, color=(0, 229, 255)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    r = size // 2 - 2
    for angle in [0, 60, 120, 180, 240, 300]:
        rad = math.radians(angle)
        x2 = cx + r * math.cos(rad)
        y2 = cy + r * math.sin(rad)
        d.line([(cx, cy), (x2, y2)], fill=(*color, 255), width=2)
        # branches
        for d_branch in [0.45, 0.75]:
            bx = cx + r * d_branch * math.cos(rad)
            by = cy + r * d_branch * math.sin(rad)
            for side in [-math.pi/4, math.pi/4]:
                brx = bx + 3.5 * math.cos(rad + side)
                bry = by + 3.5 * math.sin(rad + side)
                d.line([(bx, by), (brx, bry)], fill=(*color, 255), width=1)
    d.ellipse([cx-2, cy-2, cx+2, cy+2], fill=(*color, 255))
    return img

# 2. Leaf (Eco)
def make_leaf(size=24, color=(34, 197, 94)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Curved leaf path
    # Using polygon + bezier-like points
    pts = [
        (4, size - 4), (7, size - 8), (8, 12), (14, 5), (size - 4, 4),
        (size - 6, 12), (18, 18), (12, size - 7), (4, size - 4)
    ]
    d.polygon(pts, fill=(*color, 240), outline=(*color, 255))
    # stem / central vein
    d.line([(4, size - 4), (size - 5, 5)], fill=(255, 255, 255, 220), width=1)
    d.line([(10, 14), (14, 11)], fill=(255, 255, 255, 180), width=1)
    d.line([(13, 11), (17, 9)], fill=(255, 255, 255, 180), width=1)
    return img

# 3. Water Droplet
def make_droplet(size=24, color=(56, 189, 248)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = size // 2
    # Bottom circle
    d.ellipse([cx - 7, size - 16, cx + 7, size - 2], fill=(*color, 240))
    # Top triangle
    d.polygon([(cx, 3), (cx - 7, size - 11), (cx + 7, size - 11)], fill=(*color, 240))
    # Specular curve
    d.arc([cx - 5, size - 14, cx + 3, size - 5], 180, 270, fill=(255, 255, 255, 240), width=2)
    return img

# 4. Filter / Air Purifier
def make_filter(size=24, color=(0, 229, 255)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # 3 stacked air slats with flow
    for y in [5, 11, 17]:
        d.rounded_rectangle([3, y, size - 4, y + 3], radius=1, fill=(*color, 240))
    # dots in between slats
    for x in [6, 11, 16, 21]:
        d.ellipse([x-1, 9, x, 10], fill=(255, 255, 255, 220))
        d.ellipse([x-1, 15, x, 16], fill=(255, 255, 255, 220))
    return img

# 5. Lock
def make_lock(size=24, color=(245, 158, 11)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = size // 2
    # Shackle (arch)
    d.arc([cx - 5, 3, cx + 5, 15], 180, 360, fill=(*color, 255), width=2)
    d.line([(cx - 5, 9), (cx - 5, 12)], fill=(*color, 255), width=2)
    d.line([(cx + 5, 9), (cx + 5, 12)], fill=(*color, 255), width=2)
    # Body
    d.rounded_rectangle([cx - 7, 11, cx + 7, size - 4], radius=2, fill=(*color, 240))
    # Keyhole
    d.ellipse([cx - 1, 14, cx + 1, 16], fill=(15, 23, 42, 255))
    d.line([(cx, 16), (cx, 18)], fill=(15, 23, 42, 255), width=1)
    return img

# 6. Ice Bucket
def make_ice_bucket(size=24, color=(56, 189, 248)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = size // 2
    # Bucket shape
    d.polygon([(cx - 7, 9), (cx + 7, 9), (cx + 5, size - 4), (cx - 5, size - 4)], fill=(*color, 220), outline=(*color, 255))
    # Handle / rim
    d.line([(cx - 8, 9), (cx + 8, 9)], fill=(*color, 255), width=2)
    # Ice cubes floating
    d.rectangle([cx - 5, 4, cx - 1, 8], outline=(255, 255, 255, 255), fill=(200, 240, 255, 220))
    d.rectangle([cx + 1, 3, cx + 5, 7], outline=(255, 255, 255, 255), fill=(200, 240, 255, 220))
    # Sparkle rays
    d.line([(cx, 2), (cx, 4)], fill=(255, 255, 255, 255), width=1)
    d.line([(cx - 8, 4), (cx - 6, 6)], fill=(255, 255, 255, 255), width=1)
    d.line([(cx + 8, 4), (cx + 6, 6)], fill=(255, 255, 255, 255), width=1)
    return img

# 7. Bell
def make_bell(size=24, color=(251, 191, 36)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = size // 2
    # Top knob
    d.ellipse([cx - 2, 2, cx + 2, 5], fill=(*color, 255))
    # Bell body
    d.polygon([(cx, 5), (cx - 6, 14), (cx - 8, 17), (cx + 8, 17), (cx + 6, 14)], fill=(*color, 240))
    d.rounded_rectangle([cx - 8, 16, cx + 8, 18], radius=1, fill=(*color, 255))
    # Clapper
    d.ellipse([cx - 2, 18, cx + 2, 21], fill=(*color, 255))
    return img

# 8. Shield Check
def make_shield_check(size=20, color=(34, 197, 94)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = size // 2
    pts = [(cx, 2), (size - 3, 4), (size - 3, 10), (cx, size - 2), (3, 10), (3, 4)]
    d.polygon(pts, outline=(*color, 255), fill=(*color, 40))
    # Checkmark
    d.line([(5, 10), (cx - 1, 14)], fill=(*color, 255), width=2)
    d.line([(cx - 1, 14), (size - 6, 6)], fill=(*color, 255), width=2)
    return img

# 9. Camera Outline
def make_camera(size=28, color=(0, 229, 255)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    # Camera body
    d.rounded_rectangle([cx - 10, cy - 7, cx + 10, cy + 9], radius=3, outline=(*color, 255), width=2)
    # Top flash / prism
    d.polygon([(cx - 5, cy - 7), (cx - 3, cy - 10), (cx + 3, cy - 10), (cx + 5, cy - 7)], fill=(*color, 255))
    # Lens
    d.ellipse([cx - 4, cy - 3, cx + 4, cy + 5], outline=(*color, 255), width=2)
    d.ellipse([cx - 1, cy, cx + 1, cy + 2], fill=(*color, 255))
    # Concentric wave arcs
    d.arc([1, 1, size - 2, size - 2], 135, 225, fill=(*color, 120), width=1)
    d.arc([1, 1, size - 2, size - 2], 315, 405, fill=(*color, 120), width=1)
    return img

# 10. Custom Mode (Calendar / gear)
def make_custom_mode(size=24, color=(56, 189, 248)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    # Calendar outline
    d.rounded_rectangle([3, 4, size - 4, size - 4], radius=2, outline=(*color, 255), width=2)
    d.line([(3, 9), (size - 4, 9)], fill=(*color, 255), width=1)
    # top tabs
    d.line([(7, 2), (7, 5)], fill=(*color, 255), width=2)
    d.line([(size - 8, 2), (size - 8, 5)], fill=(*color, 255), width=2)
    # inner snowflake star
    d.line([(cx, cy + 2), (cx, cy + 6)], fill=(255, 255, 255, 255), width=1)
    d.line([(cx - 2, cy + 4), (cx + 2, cy + 4)], fill=(255, 255, 255, 255), width=1)
    d.line([(cx - 2, cy + 2), (cx + 2, cy + 6)], fill=(255, 255, 255, 255), width=1)
    d.line([(cx - 2, cy + 6), (cx + 2, cy + 2)], fill=(255, 255, 255, 255), width=1)
    return img

# 11. Nav Home
def make_nav_home(size=22, color=(0, 229, 255)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = size // 2
    # Roof
    d.polygon([(cx, 2), (size - 3, 10), (3, 10)], fill=(*color, 255))
    # Body
    d.rectangle([5, 10, size - 6, size - 3], fill=(*color, 255))
    # Door cutout
    d.rectangle([cx - 2, size - 7, cx + 2, size - 3], fill=(0, 0, 0, 0))
    return img

# 12. Nav Fridge
def make_nav_fridge(size=22, color=(0, 229, 255)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Outline 2 doors
    d.rounded_rectangle([4, 2, size - 5, 10], radius=2, outline=(*color, 255), width=1)
    d.rounded_rectangle([4, 11, size - 5, size - 3], radius=2, outline=(*color, 255), width=1)
    # Handles
    d.line([(7, 7), (7, 9)], fill=(*color, 255), width=2)
    d.line([(7, 13), (7, 16)], fill=(*color, 255), width=2)
    return img

# 13. Nav Robot
def make_nav_robot(size=22, color=(0, 229, 255)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    # Antenna
    d.line([(cx, 2), (cx, 5)], fill=(*color, 255), width=2)
    d.ellipse([cx - 2, 1, cx + 2, 4], fill=(*color, 255))
    # Head
    d.rounded_rectangle([3, 5, size - 4, size - 4], radius=3, outline=(*color, 255), width=2)
    # Eyes
    d.ellipse([6, 9, 9, 12], fill=(*color, 255))
    d.ellipse([size - 10, 9, size - 7, 12], fill=(*color, 255))
    # Smile line
    d.arc([cx - 4, cy + 2, cx + 4, cy + 6], 0, 180, fill=(*color, 255), width=1)
    # Ears
    d.line([(1, 9), (3, 9)], fill=(*color, 255), width=2)
    d.line([(size - 3, 9), (size - 1, 9)], fill=(*color, 255), width=2)
    return img

# 14. Nav Gear
def make_nav_gear(size=22, color=(0, 229, 255)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    r_out, r_in = size // 2 - 2, 4
    # 8 teeth
    for a in range(0, 360, 45):
        rad = math.radians(a)
        tx = cx + r_out * math.cos(rad)
        ty = cy + r_out * math.sin(rad)
        d.line([(cx, cy), (tx, ty)], fill=(*color, 255), width=4)
    d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], fill=(*color, 255))
    d.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=(0, 0, 0, 0))
    return img

# 15. Sun Brightness
def make_sun(size=20, color=(251, 191, 36)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    d.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=(*color, 255))
    r1, r2 = 6, size // 2 - 1
    for a in range(0, 360, 45):
        rad = math.radians(a)
        d.line([(cx + r1 * math.cos(rad), cy + r1 * math.sin(rad)),
                (cx + r2 * math.cos(rad), cy + r2 * math.sin(rad))], fill=(*color, 255), width=2)
    return img

# 16. Speaker Sound
def make_speaker(size=20, color=(168, 85, 247)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Speaker cone
    d.rectangle([2, 7, 5, 13], fill=(*color, 255))
    d.polygon([(5, 7), (10, 3), (10, 17), (5, 13)], fill=(*color, 255))
    # Sound waves
    d.arc([7, 6, 13, 14], 300, 420, fill=(*color, 255), width=2)
    d.arc([9, 3, 17, 17], 300, 420, fill=(*color, 255), width=2)
    return img

# 17. Info Icon
def make_info(size=20, color=(0, 229, 255)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    d.ellipse([2, 2, size - 3, size - 3], outline=(*color, 255), width=2)
    d.ellipse([cx - 1, 5, cx + 1, 7], fill=(*color, 255))
    d.line([(cx, 9), (cx, size - 6)], fill=(*color, 255), width=2)
    d.line([(cx - 2, 9), (cx, 9)], fill=(*color, 255), width=1)
    d.line([(cx - 2, size - 6), (cx + 2, size - 6)], fill=(*color, 255), width=1)
    return img

# 18. Language Icon
def make_lang(size=20, color=(168, 85, 247)):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2, 2, size - 3, size - 3], radius=3, outline=(*color, 255), width=1)
    # Draw 'A'
    d.line([(5, 15), (8, 6), (11, 15)], fill=(*color, 255), width=1)
    d.line([(6, 12), (10, 12)], fill=(*color, 255), width=1)
    # Asian character strokes on right
    d.line([(12, 8), (17, 8)], fill=(*color, 255), width=1)
    d.line([(14, 8), (14, 15)], fill=(*color, 255), width=1)
    d.line([(12, 11), (17, 14)], fill=(*color, 255), width=1)
    return img

# 19. Neon Wireframe Fridge for Device Info Card (48x64)
def make_fridge_wireframe(w=48, h=64, color=(0, 229, 255)):
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Main outer body with neon glow
    d.rounded_rectangle([4, 2, w - 5, h - 3], radius=4, outline=(*color, 255), width=2)
    # Upper French doors split
    mid_y = int(h * 0.56)
    mid_x = w // 2
    # horizontal divider
    d.line([(4, mid_y), (w - 5, mid_y)], fill=(*color, 255), width=2)
    # top vertical divider (French doors)
    d.line([(mid_x, 2), (mid_x, mid_y)], fill=(*color, 255), width=2)
    # bottom freezer vertical divider or drawers
    d.line([(mid_x, mid_y), (mid_x, h - 3)], fill=(*color, 255), width=1)
    # Left door water dispenser
    d.rectangle([9, 14, mid_x - 5, 26], outline=(*color, 200), fill=(*color, 50))
    # Right door smart LCD screen
    d.rectangle([mid_x + 5, 10, w - 9, 32], outline=(*color, 255), fill=(*color, 80))
    # screen mini icons dots
    for sx in range(mid_x + 8, w - 10, 4):
        for sy in range(14, 30, 5):
            d.point((sx, sy), fill=(255, 255, 255, 255))
    # handles
    d.line([(mid_x - 2, 18), (mid_x - 2, mid_y - 4)], fill=(255, 255, 255, 255), width=1)
    d.line([(mid_x + 2, 18), (mid_x + 2, mid_y - 4)], fill=(255, 255, 255, 255), width=1)
    d.line([(mid_x - 2, mid_y + 6), (mid_x - 2, h - 10)], fill=(255, 255, 255, 255), width=1)
    d.line([(mid_x + 2, mid_y + 6), (mid_x + 2, h - 10)], fill=(255, 255, 255, 255), width=1)
    return img

def make_minimazing_badge(size=24):
    scale = 4
    s = size * scale
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = s / 2, s / 2
    r = (s / 2) - (2 * scale)
    # Outer circle
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 255, 255, 255), width=int(1.6 * scale))
    # Chimney
    d.rectangle([int(15 * scale), int(6.5 * scale), int(16.5 * scale), int(10 * scale)], fill=(255, 255, 255, 255))
    # Roof
    d.line([(int(4.5 * scale), int(12 * scale)), (int(12 * scale), int(5.5 * scale)), (int(19.5 * scale), int(12 * scale))], fill=(255, 255, 255, 255), width=int(1.6 * scale))
    # Walls
    d.line([(int(6.5 * scale), int(12 * scale)), (int(6.5 * scale), int(18.5 * scale))], fill=(255, 255, 255, 255), width=int(1.6 * scale))
    d.line([(int(17.5 * scale), int(12 * scale)), (int(17.5 * scale), int(18.5 * scale))], fill=(255, 255, 255, 255), width=int(1.6 * scale))
    # Bottom
    d.line([(int(6.5 * scale), int(18.5 * scale)), (int(10.5 * scale), int(18.5 * scale))], fill=(255, 255, 255, 255), width=int(1.6 * scale))
    d.line([(int(13.5 * scale), int(18.5 * scale)), (int(17.5 * scale), int(18.5 * scale))], fill=(255, 255, 255, 255), width=int(1.6 * scale))
    # Door
    d.line([(int(10.5 * scale), int(18.5 * scale)), (int(10.5 * scale), int(14 * scale)), (int(13.5 * scale), int(14 * scale)), (int(13.5 * scale), int(18.5 * scale))], fill=(255, 255, 255, 255), width=int(1.4 * scale))
    # Cord
    d.arc([int(13.5 * scale), int(15 * scale), int(18.5 * scale), int(19 * scale)], 270, 90, fill=(255, 255, 255, 255), width=int(1.4 * scale))
    d.arc([int(17.5 * scale), int(14 * scale), int(21 * scale), int(18 * scale)], 90, 270, fill=(255, 255, 255, 255), width=int(1.4 * scale))
    return img.resize((size, size), Image.Resampling.LANCZOS)

def main():
    icons = {
        "icon_minimazing_badge": make_minimazing_badge(24),
        "icon_snowflake_blue": make_snowflake(24, (0, 229, 255)),
        "icon_leaf_green": make_leaf(24, (34, 197, 94)),
        "icon_droplet_blue": make_droplet(24, (56, 189, 248)),
        "icon_filter_cyan": make_filter(24, (0, 229, 255)),
        "icon_lock_amber": make_lock(24, (245, 158, 11)),
        "icon_ice_bucket": make_ice_bucket(24, (56, 189, 248)),
        "icon_bell_yellow": make_bell(24, (251, 191, 36)),
        "icon_shield_green": make_shield_check(20, (34, 197, 94)),
        "icon_camera_cyan": make_camera(28, (0, 229, 255)),
        "icon_custom_mode": make_custom_mode(24, (56, 189, 248)),
        "icon_nav_home_act": make_nav_home(22, (0, 229, 255)),
        "icon_nav_home_inact": make_nav_home(22, (100, 116, 139)),
        "icon_nav_fridge_act": make_nav_fridge(22, (0, 229, 255)),
        "icon_nav_fridge_inact": make_nav_fridge(22, (100, 116, 139)),
        "icon_nav_robot_act": make_nav_robot(22, (0, 229, 255)),
        "icon_nav_robot_inact": make_nav_robot(22, (100, 116, 139)),
        "icon_nav_gear_act": make_nav_gear(22, (0, 229, 255)),
        "icon_nav_gear_inact": make_nav_gear(22, (100, 116, 139)),
        "icon_sun_yellow": make_sun(20, (251, 191, 36)),
        "icon_speaker_purple": make_speaker(20, (168, 85, 247)),
        "icon_info_cyan": make_info(20, (0, 229, 255)),
        "icon_lang_purple": make_lang(20, (168, 85, 247)),
        "img_fridge_neon": make_fridge_wireframe(48, 64, (0, 229, 255)),
    }

    out_c = ["#pragma once\n#include <lvgl.h>\n\n"]
    for name, img in icons.items():
        out_c.append(export_image_to_c(img, name, with_alpha=True))
        
    target_path = os.path.join("include", "fridge_icons.h")
    with open(target_path, "w", encoding="utf-8") as f:
        f.write("".join(out_c))
        
    print(f"Generated {len(icons)} icons into {target_path}")

if __name__ == "__main__":
    main()
