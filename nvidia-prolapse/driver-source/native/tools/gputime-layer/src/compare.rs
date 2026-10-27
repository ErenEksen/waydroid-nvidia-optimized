//! gputime-compare: two VK_LAYER_PERF_gputime CSVs (A = ours, B = reference) -> regions matched by identity,
//! median GPU ns per region over the frames, delta A - B, sorted by delta, as % of B's median frame busy time.
//!
//!   gputime-compare A.csv B.csv [--strict] [--top N] [--csv out.csv]
//!
//! Identity = kind, attachment formats, extent (render area / dispatch size), for dispatches the pipeline
//! hash, plus the occurrence number of that identity within its frame (a frame that runs the same pass twice
//! gives two regions). A pass's pipeline sequence and draw count are reported, not matched: a time-based
//! animation draws a different number of objects in frame N on a faster driver. --strict also matches them.
//! Only frames present in both files are used.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs;
use std::process::exit;

type Key = (String, String, String, String, String, u32);

struct Side {
    regions: HashMap<Key, BTreeMap<u64, u64>>, // key -> frame -> ns
    draws: HashMap<Key, BTreeMap<u64, u64>>,   // key -> frame -> draws
    busy: BTreeMap<u64, u64>,
    region_sum: BTreeMap<u64, u64>,
    device: String,
}

fn load(path: &str, strict: bool) -> Side {
    let text = fs::read_to_string(path).unwrap_or_else(|e| {
        eprintln!("gputime-compare: cannot read {path}: {e}");
        exit(2)
    });
    let mut s = Side { regions: HashMap::new(), draws: HashMap::new(), busy: BTreeMap::new(), region_sum: BTreeMap::new(), device: String::new() };
    let mut occ: HashMap<(u64, (String, String, String, String, String)), u32> = HashMap::new();
    let mut header: Option<Vec<String>> = None;
    for (n, line) in text.lines().enumerate() {
        if let Some(d) = line.strip_prefix("# device=") {
            s.device = d.split(" vendor=").next().unwrap_or(d).to_string();
            continue;
        }
        if line.starts_with('#') || line.is_empty() {
            continue;
        }
        let f: Vec<&str> = line.split(',').collect();
        let Some(h) = header.as_ref() else {
            header = Some(f.iter().map(|x| x.to_string()).collect());
            continue;
        };
        let col = |name: &str| -> &str {
            let i = h.iter().position(|c| c == name).unwrap_or_else(|| {
                eprintln!("gputime-compare: {path}: no column {name}");
                exit(2)
            });
            f.get(i).copied().unwrap_or("")
        };
        let num = |name: &str| -> u64 {
            col(name).parse().unwrap_or_else(|_| {
                eprintln!("gputime-compare: {path}:{}: column {name} is not a number: {line}", n + 1);
                exit(2)
            })
        };
        let frame = num("frame");
        match col("kind") {
            "frame" => {
                s.busy.insert(frame, num("gpu_ns"));
            }
            "cb" => {}
            kind => {
                let ns = num("gpu_ns");
                let dispatch = kind == "cs" || kind == "csi";
                let id = (
                    kind.to_string(),
                    col("fmt").to_string(),
                    col("extent").to_string(),
                    if strict || dispatch { col("pipe").to_string() } else { String::new() },
                    if strict { col("draws").to_string() } else { String::new() },
                );
                let o = occ.entry((frame, id.clone())).or_insert(0);
                let key = (id.0, id.1, id.2, id.3, id.4, *o);
                *o += 1;
                s.draws.entry(key.clone()).or_default().insert(frame, num("draws"));
                s.regions.entry(key).or_default().insert(frame, ns);
                *s.region_sum.entry(frame).or_insert(0) += ns;
            }
        }
    }
    s
}

fn median(mut v: Vec<u64>) -> Option<f64> {
    if v.is_empty() {
        return None;
    }
    v.sort_unstable();
    let n = v.len();
    Some(if n % 2 == 1 { v[n / 2] as f64 } else { (v[n / 2 - 1] + v[n / 2]) as f64 / 2.0 })
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut files = Vec::new();
    let (mut strict, mut top, mut csv) = (false, usize::MAX, None::<String>);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--strict" => strict = true,
            "--top" => top = it.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| usage()),
            "--csv" => csv = Some(it.next().cloned().unwrap_or_else(|| usage())),
            _ if a.starts_with("--") => usage(),
            _ => files.push(a.clone()),
        }
    }
    if files.len() != 2 {
        usage();
    }
    let a = load(&files[0], strict);
    let b = load(&files[1], strict);
    let frames: BTreeSet<u64> = a.busy.keys().filter(|f| b.busy.contains_key(f)).copied().collect();
    if frames.is_empty() {
        eprintln!("gputime-compare: no frame is present in both files (frame rows)");
        exit(1);
    }
    let in_frames = |m: &BTreeMap<u64, u64>| -> Vec<u64> {
        m.iter().filter(|(f, _)| frames.contains(f)).map(|(_, v)| *v).collect()
    };
    let busy_a = median(in_frames(&a.busy)).unwrap();
    let busy_b = median(in_frames(&b.busy)).unwrap();
    let other = |s: &Side| {
        median(frames.iter().map(|f| s.busy[f].saturating_sub(*s.region_sum.get(f).unwrap_or(&0))).collect()).unwrap()
    };
    let (other_a, other_b) = (other(&a), other(&b));

    struct Row {
        key: Key,
        ma: Option<f64>,
        mb: Option<f64>,
        da: Option<f64>,
        db: Option<f64>,
        na: usize,
        nb: usize,
    }
    let keys: BTreeSet<&Key> = a.regions.keys().chain(b.regions.keys()).collect();
    let mut rows: Vec<Row> = keys
        .into_iter()
        .map(|k| {
            let va = a.regions.get(k).map(&in_frames).unwrap_or_default();
            let vb = b.regions.get(k).map(&in_frames).unwrap_or_default();
            let da = a.draws.get(k).map(&in_frames).and_then(median);
            let db = b.draws.get(k).map(&in_frames).and_then(median);
            Row { key: k.clone(), na: va.len(), nb: vb.len(), ma: median(va), mb: median(vb), da, db }
        })
        .filter(|r| r.na > 0 || r.nb > 0)
        .collect();
    let delta = |r: &Row| r.ma.unwrap_or(0.0) - r.mb.unwrap_or(0.0);
    rows.sort_by(|x, y| delta(y).total_cmp(&delta(x)));

    println!("A: {} ({})\nB: {} ({})", files[0], a.device, files[1], b.device);
    println!("frames in both: {} ({}..={})", frames.len(), frames.first().unwrap(), frames.last().unwrap());
    println!(
        "median frame GPU busy: A {:.0} ns, B {:.0} ns, delta {:+.0} ns ({:+.2} % of B)",
        busy_a,
        busy_b,
        busy_a - busy_b,
        100.0 * (busy_a - busy_b) / busy_b
    );
    println!(
        "median outside regions (busy - sum of regions): A {other_a:.0} ns, B {other_b:.0} ns, delta {:+.0} ns",
        other_a - other_b
    );
    let matched: Vec<&Row> = rows.iter().filter(|r| r.ma.is_some() && r.mb.is_some()).collect();
    let only_a: f64 = rows.iter().filter(|r| r.mb.is_none()).map(|r| r.ma.unwrap_or(0.0)).sum();
    let only_b: f64 = rows.iter().filter(|r| r.ma.is_none()).map(|r| r.mb.unwrap_or(0.0)).sum();
    println!(
        "regions: {} matched, {} only in A (sum of medians {only_a:.0} ns), {} only in B ({only_b:.0} ns)",
        matched.len(),
        rows.iter().filter(|r| r.mb.is_none()).count(),
        rows.iter().filter(|r| r.ma.is_none()).count()
    );
    println!();
    println!(
        "{:>4} {:<4} {:<20} {:<11} {:<16} {:>9} {:>3} {:>10} {:>10} {:>10} {:>7} {:>7} {:>7}",
        "rank", "kind", "fmt", "extent", "pipe", "draws A/B", "occ", "A_ns", "B_ns", "delta_ns", "%Bframe", "A/B", "n"
    );
    let mut out = String::from(
        "rank,kind,fmt,extent,pipe,draws_a,draws_b,occ,a_ns,b_ns,delta_ns,pct_b_frame,ratio,frames_a,frames_b\n",
    );
    for (i, r) in rows.iter().enumerate() {
        let (k, f, e, p, _, o) = &r.key;
        let dr = |v: Option<f64>| v.map(|x| format!("{x:.0}")).unwrap_or_else(|| "-".into());
        let d = format!("{}/{}", dr(r.da), dr(r.db));
        let fs = |v: Option<f64>| v.map(|x| format!("{x:.0}")).unwrap_or_else(|| "-".into());
        let ratio = match (r.ma, r.mb) {
            (Some(x), Some(y)) if y > 0.0 => format!("{:.3}", x / y),
            _ => "-".into(),
        };
        let pct = 100.0 * delta(r) / busy_b;
        if i < top {
            println!(
                "{:>4} {:<4} {:<20} {:<11} {:<16} {:>9} {:>3} {:>10} {:>10} {:>+10.0} {:>+7.2} {:>7} {:>7}",
                i + 1,
                k,
                f,
                e,
                p,
                d,
                o,
                fs(r.ma),
                fs(r.mb),
                delta(r),
                pct,
                ratio,
                format!("{}/{}", r.na, r.nb)
            );
        }
        out.push_str(&format!(
            "{},{k},{f},{e},{p},{},{},{o},{},{},{:.0},{pct:.3},{ratio},{},{}\n",
            i + 1,
            dr(r.da),
            dr(r.db),
            fs(r.ma),
            fs(r.mb),
            delta(r),
            r.na,
            r.nb
        ));
    }
    if let Some(path) = csv {
        if let Err(e) = fs::write(&path, out) {
            eprintln!("gputime-compare: cannot write {path}: {e}");
            exit(2);
        }
    }
}

fn usage() -> ! {
    eprintln!("usage: gputime-compare A.csv B.csv [--strict] [--top N] [--csv out.csv]");
    exit(2)
}
