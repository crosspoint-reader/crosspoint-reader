from defusedxml import ElementTree as ET


def parse_svg_intrinsic_size(svg_data):
    try:
        root = ET.fromstring(svg_data)
    except ET.ParseError:
        return None, None

    viewbox = root.get("viewBox") or root.get("viewbox")
    if viewbox:
        parts = viewbox.replace(",", " ").split()
        if len(parts) == 4:
            try:
                vb_w = float(parts[2])
                vb_h = float(parts[3])
                if vb_w > 0 and vb_h > 0:
                    return vb_w, vb_h
            except ValueError:
                pass

    def parse_len(value):
        if not value:
            return None
        cleaned = "".join(ch for ch in value if ch.isdigit() or ch in ".-")
        if not cleaned:
            return None
        try:
            parsed = float(cleaned)
            return parsed if parsed > 0 else None
        except ValueError:
            return None

    w = parse_len(root.get("width"))
    h = parse_len(root.get("height"))
    return w, h


def fit_inside_canvas(src_w, src_h, dst_w, dst_h):
    if src_w <= 0 or src_h <= 0 or dst_w <= 0 or dst_h <= 0:
        return dst_w, dst_h

    src_ratio = src_w / src_h
    dst_ratio = dst_w / dst_h

    if src_ratio >= dst_ratio:
        fit_w = dst_w
        fit_h = max(1, round(fit_w / src_ratio))
    else:
        fit_h = dst_h
        fit_w = max(1, round(fit_h * src_ratio))

    return fit_w, fit_h
