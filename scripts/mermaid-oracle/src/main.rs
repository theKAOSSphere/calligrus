//! Oracle for the C++ Mermaid port: writes exactly the SVG text that
//! the reference implementation hands to resvg (render_with_options + fix_svg).
//!
//! Usage: mermaid-oracle [--raw] FILE.mmd...   -> writes FILE.svg next to each input.
//!   --raw  skip fix_svg (the crate's own output).
//! `ink_theme`, `attr` and `fix_svg` are COPIES of the reference implementation; if that file
//! changes, re-copy them (the oracle must match the app).
use mermaid_rs_renderer::{RenderOptions, Theme};
use resvg::{tiny_skia, usvg};

const FONT_FAMILY: &str = "IBM Plex Sans";

/// Value of attribute `name` inside a single tag string.
fn attr<'a>(tag: &'a str, name: &str) -> Option<&'a str> {
    let key = format!(" {name}=\"");
    let i = tag.find(&key)? + key.len();
    let j = tag[i..].find('"')? + i;
    Some(&tag[i..j])
}

/// Post-process the renderer's SVG to work around two quirks:
/// 1. `((circle))` nodes come out as two concentric circles: drop the inner
///    `fill="none"` ring that directly follows a circle with the same centre.
/// 2. Subgraph titles are drawn before edges, so edges cross the text: move
///    each title to the end of the document on a box filled like the cluster.
fn fix_svg(svg: &str) -> String {
    let mut out = String::with_capacity(svg.len() + 256);
    let mut titles = String::new();
    let mut rest = svg;
    let mut prev_circle: Option<(String, String)> = None;
    let mut cluster_fill: Option<String> = None;
    while let Some(lt) = rest.find('<') {
        out.push_str(&rest[..lt]);
        rest = &rest[lt..];
        let Some(gt) = rest.find('>') else { break };
        let tag = &rest[..=gt];
        if tag.starts_with("<circle") {
            let c = (attr(tag, "cx").unwrap_or("").to_string(), attr(tag, "cy").unwrap_or("").to_string());
            let dup = attr(tag, "fill") == Some("none") && prev_circle.as_ref() == Some(&c);
            prev_circle = Some(c);
            cluster_fill = None;
            rest = &rest[gt + 1..];
            if !dup {
                out.push_str(tag);
            }
            continue;
        }
        if tag.starts_with("<text") {
            if let (Some(fill), Some(end)) = (cluster_fill.take(), rest.find("</text>")) {
                let whole = &rest[..end + "</text>".len()];
                let x: f32 = attr(tag, "x").and_then(|v| v.parse().ok()).unwrap_or(0.0);
                let y: f32 = attr(tag, "y").and_then(|v| v.parse().ok()).unwrap_or(0.0);
                let fs: f32 = attr(tag, "font-size").and_then(|v| v.parse().ok()).unwrap_or(14.0);
                // Visible characters only (strip nested tags).
                let mut n = 0usize;
                let mut in_tag = false;
                for ch in whole[gt + 1..].chars() {
                    match ch {
                        '<' => in_tag = true,
                        '>' => in_tag = false,
                        _ if !in_tag => n += 1,
                        _ => {}
                    }
                }
                let w = n as f32 * fs * 0.6 + 8.0;
                let h = fs * 1.3;
                titles.push_str(&format!(
                    "<rect x=\"{:.2}\" y=\"{:.2}\" width=\"{w:.2}\" height=\"{h:.2}\" fill=\"{fill}\" stroke=\"none\"/>{whole}",
                    x - w / 2.0,
                    y - fs * 1.0,
                ));
                rest = &rest[whole.len()..];
                prev_circle = None;
                continue;
            }
        }
        // A subgraph box: rounded rect with a stroke, followed by its title.
        cluster_fill = if tag.starts_with("<rect") && attr(tag, "rx") == Some("10") && attr(tag, "stroke").is_some() {
            attr(tag, "fill").map(str::to_string)
        } else {
            None
        };
        prev_circle = None;
        out.push_str(tag);
        rest = &rest[gt + 1..];
    }
    out.push_str(rest);
    if !titles.is_empty() {
        if let Some(i) = out.rfind("</svg>") {
            out.insert_str(i, &titles);
        }
    }
    out
}

/// A black-and-white theme: black lines and text, white backgrounds, pale-grey
/// node fills. After `ink()` black becomes full ink, white becomes nothing and
/// the grey becomes a faint wash of the text colour behind each node.
fn ink_theme() -> Theme {
    let mut t = Theme::modern();
    let black = "#000000".to_string();
    let white = "#FFFFFF".to_string();
    let fill = "#EBEBEB".to_string();
    t.font_family = format!("\"{FONT_FAMILY}\", sans-serif");
    t.primary_color = fill.clone();
    t.secondary_color = fill.clone();
    t.tertiary_color = white.clone();
    t.primary_text_color = black.clone();
    t.text_color = black.clone();
    t.primary_border_color = black.clone();
    t.line_color = black.clone();
    t.edge_label_background = white.clone();
    t.cluster_background = "#F5F5F5".to_string();
    t.cluster_border = "#808080".to_string();
    t.background = white.clone();
    t.sequence_actor_fill = fill.clone();
    t.sequence_actor_border = black.clone();
    t.sequence_actor_line = "#606060".to_string();
    t.sequence_note_fill = "#F5F5F5".to_string();
    t.sequence_note_border = "#606060".to_string();
    t.sequence_activation_fill = fill;
    t.sequence_activation_border = black.clone();
    t.pie_title_text_color = black.clone();
    t.pie_section_text_color = black.clone();
    t.pie_legend_text_color = black.clone();
    t.pie_stroke_color = black;
    t
}

/// The crate measures text with whatever font it finds on THIS machine (system fonts, or a
/// disk cache in $XDG_CACHE_HOME/mmdr/font-cache keyed by a hash of the font-family string),
/// so its layout differs from machine to machine. To make goldens deterministic we pre-seed that
/// cache with the bundled IBM Plex Sans (the font the picture is drawn with). The path code below
/// copies `cache_paths` from the crate's text_metrics.rs.
fn seed_font_cache(theme_family: &str) {
    use std::hash::{Hash, Hasher};
    let dir = std::env::current_dir().unwrap().join("target").join("fontcache");
    unsafe { std::env::set_var("XDG_CACHE_HOME", &dir) };
    let key = format!("v2-font-family-case:{}", theme_family.trim());
    let mut h = std::collections::hash_map::DefaultHasher::new();
    key.hash(&mut h);
    let d = dir.join("mmdr").join("font-cache");
    std::fs::create_dir_all(&d).unwrap();
    let font = include_bytes!("../../../assets/fonts/IBMPlexSans-Regular.ttf");
    std::fs::write(d.join(format!("{:x}.font", h.finish())), font).unwrap();
    std::fs::write(d.join(format!("{:x}.meta", h.finish())), "0").unwrap();
}

/// Debug oracle for the layout stage: X.layout.txt with node boxes and edge polylines
/// (f32 printed with Rust's shortest round-trip Display, so C++ std::to_chars can match).
fn write_layout_text(src: &str, mmd_path: &str) {
    let Ok(parsed) = mermaid_rs_renderer::parse_mermaid(src) else { return };
    let o = RenderOptions { theme: ink_theme(), ..RenderOptions::default() };
    let l = mermaid_rs_renderer::compute_layout(&parsed.graph, &o.theme, &o.layout);
    let mut t = format!("size {} {}
", l.width, l.height);
    for n in l.nodes.values() {
        t += &format!("node {} {} {} {} {} hidden={} lines={:?} label={}x{}
", n.id, n.x, n.y, n.width, n.height, n.hidden, n.label.lines, n.label.width, n.label.height);
    }
    for e in &l.edges {
        let pts: Vec<String> = e.points.iter().map(|(x, y)| format!("({x},{y})")).collect();
        t += &format!("edge {}->{} {} anchor={:?}
", e.from, e.to, pts.join(" "), e.label_anchor);
    }
    for s in &l.subgraphs {
        t += &format!("sub {:?} {} {} {} {}
", s.label, s.x, s.y, s.width, s.height);
    }
    let out = std::path::Path::new(mmd_path).with_extension("layout.txt");
    std::fs::write(out, t).unwrap();
}

/// Rasterise SVG text exactly like the reference implementation `render_inner` (same fonts, ink()).
/// Returns (width, height, ink alpha bytes).
fn rasterise(svg: &str, scale: f32) -> Option<(u32, u32, Vec<u8>)> {
    let mut db = usvg::fontdb::Database::new();
    for f in ["IBMPlexSans-Regular.ttf", "IBMPlexSans-Bold.ttf", "IBMPlexSans-Italic.ttf"] {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../assets/fonts").join(f);
        db.load_font_data(std::fs::read(path).ok()?);
    }
    db.set_sans_serif_family(FONT_FAMILY);
    let mut opt = usvg::Options::default();
    opt.fontdb = std::sync::Arc::new(db);
    opt.font_family = FONT_FAMILY.to_string();
    let tree = usvg::Tree::from_str(svg, &opt).ok()?;
    let size = tree.size();
    let width = (size.width() * scale).ceil() as u32;
    let height = (size.height() * scale).ceil() as u32;
    let mut pixmap = tiny_skia::Pixmap::new(width, height)?;
    resvg::render(&tree, tiny_skia::Transform::from_scale(scale, scale), &mut pixmap.as_mut());
    // ink(): copy of the function in the reference implementation
    let mut out = Vec::with_capacity((width * height) as usize);
    for px in pixmap.data().chunks_exact(4) {
        let a = px[3] as f32 / 255.0;
        let lum = 0.299 * px[0] as f32 + 0.587 * px[1] as f32 + 0.114 * px[2] as f32;
        out.push((a * 255.0 - lum).clamp(0.0, 255.0).round() as u8);
    }
    Some((width, height, out))
}

fn main() {
    let mut raw = false;
    let mut masks: Option<(std::path::PathBuf, f32)> = None;
    if std::env::var_os("ORACLE_SYSTEM_FONTS").is_none() {
        seed_font_cache(&ink_theme().font_family);
        // C4 diagrams measure with their own family string (config.c4.*_font_family); seed it too.
        seed_font_cache("\"Open Sans\", sans-serif");
    }
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        if a == "--raw" { raw = true; continue; }
        // --masks DIR SCALE: also write DIR/<name>.mask (u32 w, u32 h little endian, then w*h ink bytes).
        if a == "--masks" {
            let dir = std::path::PathBuf::from(args.next().expect("--masks DIR SCALE"));
            let scale: f32 = args.next().expect("--masks DIR SCALE").parse().expect("scale");
            std::fs::create_dir_all(&dir).unwrap();
            masks = Some((dir, scale));
            continue;
        }
        let src = std::fs::read_to_string(&a).expect("read .mmd");
        let options = RenderOptions { theme: ink_theme(), ..RenderOptions::default() };
        match mermaid_rs_renderer::render_with_options(&src, options) {
            Ok(svg) => {
                write_layout_text(&src, &a);
                let svg = if raw { svg } else { fix_svg(&svg) };
                if let Some((dir, scale)) = &masks {
                    if let Some((w, h, ink)) = rasterise(&svg, *scale) {
                        let mut bytes = Vec::new();
                        bytes.extend_from_slice(&w.to_le_bytes());
                        bytes.extend_from_slice(&h.to_le_bytes());
                        bytes.extend_from_slice(&ink);
                        let name = std::path::Path::new(&a).file_stem().unwrap().to_string_lossy().into_owned();
                        std::fs::write(dir.join(format!("{name}.mask")), bytes).unwrap();
                    }
                    continue;  // masks mode does not touch the goldens
                }
                let out = std::path::Path::new(&a).with_extension("svg");
                std::fs::write(&out, svg).expect("write svg");
            }
            Err(e) => eprintln!("{a}: {e:#}"),
        }
    }
}
