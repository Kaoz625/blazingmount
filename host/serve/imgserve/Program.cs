// imgserve - read-only HTTP Range server for files inside .ffpfsc images (tokon-mod-3, 20 Oct 2026). See imgserve.csproj.
//
//   dotnet imgserve.dll <root> [<root> ...] [--port 4451] [--bind 0.0.0.0] [--depth 4] [--cache FILE] [--locked]
//
//   GET /                                       text help + every image found under the roots (name, size, title id)
//   GET /v/<image.ffpfsc>/                      JSON list of every inner file: [{"path","size"}] (also /v/<image>/?list)
//   GET|HEAD /v/<image.ffpfsc>/<inner/path>     the file's bytes; Range: bytes=a-b | a- | -n  ->  206 + Content-Range
//   GET|HEAD /t/<TITLEID>[@<image bytes>]/<inner/path>   the same, found BY TITLE ID (smb-link-5, 21 Oct 2026): renaming or moving an
//                                               image between the roots never breaks a PS5 starter. '@<bytes>' = the image file's exact size,
//                                               needed when one title id has several images (Tokon: a PHAT5 and a DIGI5 build).
// Images are found under every root (recursive, --depth levels; a scan runs at start, again when a name or title id is not found, at
// most every 15 s). Title ids come from each image's own sce_sys/param.json (cached in --cache by path + size + mtime), else from the
// file name ("(PPSA12345)", "[PPSA12345]", "PPSA12345 ...").
// Every request is one log line (image, path, range, status, bytes, ms, path kind): the server-side trace of what a game really reads.
// Read-only by construction: images are opened read-only, the only things written are the console log and the title-id cache.
//
// FAST PATH (smb-link-4, 21 Oct 2026; exFAT inside PFSC = every .ffpfsc game image so far): the library's OpenFile builds a cluster array of the WHOLE inner
// file on every call (LINQ: ~26 ms for a 33-46 GB pak, even for a 4 KB read - that was the old per-request floor, 12-16 MB/s for 512 KB reads), and its decoded
// stream lets one reader in at a time. The fast path keeps one extent map per inner file (built once), reads the stored PFSC blocks with positional reads
// (RandomAccess.Read = pread: thread-safe, no shared position, no lock) and inflates them itself, so requests run in parallel. `--locked` = the old way
// (kept as the simple fallback; UFS2 / nested images always use it).
using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Compression;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Http;
using Microsoft.Win32.SafeHandles;
using PS5PKGTool.Ffpfsc;

var roots = new List<string>();
int port = 4451;
string bind = "0.0.0.0";
bool lockedOnly = false;
int depth = 4;
string cachePath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".blazing", "imgserve", "tid-index.json");
for (int i = 0; i < args.Length; i++)
{
    if (args[i] == "--port") port = int.Parse(args[++i]);
    else if (args[i] == "--bind") bind = args[++i];
    else if (args[i] == "--locked") lockedOnly = true;
    else if (args[i] == "--depth") depth = int.Parse(args[++i]);
    else if (args[i] == "--cache") cachePath = args[++i];
    else if (args[i] == "--root") roots.Add(Path.GetFullPath(args[++i]));
    else roots.Add(Path.GetFullPath(args[i]));
}
roots = roots.Where(Directory.Exists).Distinct().ToList();
if (roots.Count == 0) { Console.Error.WriteLine("usage: imgserve <root> [<root> ...] [--port 4451] [--bind 0.0.0.0] [--depth 4] [--cache FILE] [--locked]"); return 2; }

var volumes = new ConcurrentDictionary<string, Lazy<Vol>>(StringComparer.Ordinal);   // key = full path of the image
var catalog = new Catalog(roots, depth, cachePath);
catalog.Scan("start");

Vol GetVol(string full)
{
    return volumes.GetOrAdd(full, f => new Lazy<Vol>(() =>
    {
        var sw = Stopwatch.StartNew();
        var v = FfpfscVolume.Open(f);
        var sizes = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
        foreach (var e in v.Entries) if (!e.IsDirectory) sizes[e.Path.Replace('\\', '/').Trim('/')] = e.Size;
        FastImage? fast = null;
        if (!lockedOnly && v.InnerFilesystemKind == FfpfscInnerFilesystemKind.Exfat)
        {
            try { fast = new FastImage(f, v); }
            catch (Exception ex) { Console.WriteLine($"fast path off for {Path.GetFileName(f)}: {ex.Message}"); }
        }
        Console.WriteLine($"{DateTime.Now:HH:mm:ss} opened {f}: {sizes.Count} files, {v.InnerFilesystemKind}, {sw.ElapsedMilliseconds} ms, {(fast != null ? "fast parallel path" : "locked path")}");
        return new Vol(v, sizes, fast);
    })).Value;
}

var builder = WebApplication.CreateBuilder(new WebApplicationOptions { Args = Array.Empty<string>() });
builder.WebHost.UseUrls($"http://{bind}:{port}");
builder.WebHost.ConfigureKestrel(k => { k.Limits.MinResponseDataRate = null; k.Limits.KeepAliveTimeout = TimeSpan.FromMinutes(10); });
builder.Logging.ClearProviders();
// smb-link-6 22 Oct 2026: honest stutter signal for ps5-playwatch. Tag any served range >= SlowMs with SLOW, and print a rolling p95 of
// the last 512 request times every 10 s, so a streamed game that starves on a camera pan is visible without parsing every line.
int slowMs = int.TryParse(Environment.GetEnvironmentVariable("IMGSERVE_SLOW_MS"), out var _sm) ? _sm : 120;
var lat = new System.Collections.Generic.Queue<long>();
var latLock = new object();
var lastP95 = DateTime.MinValue;
void NoteLatency(long ms) {
    lock (latLock) {
        lat.Enqueue(ms);
        while (lat.Count > 512) lat.Dequeue();
        var now = DateTime.Now;
        if ((now - lastP95).TotalSeconds >= 10 && lat.Count >= 20) {
            lastP95 = now;
            var xs = lat.ToArray(); System.Array.Sort(xs);
            Console.WriteLine($"{now:HH:mm:ss} P95 last {xs.Length} reqs: p50 {xs[xs.Length/2]} ms p95 {xs[(int)(xs.Length*0.95)]} ms max {xs[xs.Length-1]} ms");
        }
    }
}

var app = builder.Build();

app.MapGet("/", () => Results.Text("imgserve: GET /v/<image.ffpfsc>/<inner path> or /t/<TITLEID>[@<bytes>]/<inner path> with Range (read-only)\nroots: " +
    string.Join(" | ", roots) + "\nimages:\n" + string.Join("\n", catalog.All().Select(x => $"  {x.Name}  {x.Size} B  {x.Tid ?? "?"} {x.Version ?? ""}  {x.Path}")) + "\n"));

app.MapMethods("/v/{image}/{**path}", new[] { "GET", "HEAD" }, async (HttpContext ctx, string image, string? path) =>
{
    string? full = (image.Length == 0 || image.Contains('/') || image.Contains('\\') || image.Contains("..") ||
                    !image.EndsWith(".ffpfsc", StringComparison.OrdinalIgnoreCase)) ? null : catalog.ByName(image);
    await Serve(ctx, full, image, path);
});

app.MapMethods("/t/{key}/{**path}", new[] { "GET", "HEAD" }, async (HttpContext ctx, string key, string? path) =>
{
    string? full = catalog.ByTitle(key, out string why);
    if (full == null) Console.WriteLine($"{DateTime.Now:HH:mm:ss} /t/{key}: {why}");
    await Serve(ctx, full, key, path);
});

async Task Serve(HttpContext ctx, string? full, string label, string? path)
{
    var sw = Stopwatch.StartNew();
    if (full == null || !File.Exists(full)) { ctx.Response.StatusCode = 404; return; }
    Vol vol;
    try { vol = GetVol(full); }
    catch (Exception ex) { Console.WriteLine($"open failed {full}: {ex.Message}"); ctx.Response.StatusCode = 500; return; }
    path = (path ?? "").Replace('\\', '/').Trim('/');
    if (path == "" || ctx.Request.Query.ContainsKey("list"))
    {
        ctx.Response.ContentType = "application/json";
        await ctx.Response.WriteAsync(JsonSerializer.Serialize(vol.Sizes.Select(kv => new { path = kv.Key, size = kv.Value })));
        return;
    }
    if (!vol.Sizes.TryGetValue(path, out long size)) { ctx.Response.StatusCode = 404; Console.WriteLine($"{DateTime.Now:HH:mm:ss} 404 {label}/{path}"); return; }
    long start = 0, end = size - 1;
    int status = 200;
    string rangeText = "-";
    if (ctx.Request.Headers.TryGetValue("Range", out var rh) && rh.ToString().StartsWith("bytes=", StringComparison.OrdinalIgnoreCase))
    {
        string spec = rh.ToString().Substring(6).Split(',')[0].Trim();
        int dash = spec.IndexOf('-');
        bool ok = dash >= 0;
        long a = -1, b = -1;
        if (ok)
        {
            string sa = spec[..dash], sb = spec[(dash + 1)..];
            if (sa == "" && long.TryParse(sb, out long n)) { a = Math.Max(0, size - n); b = size - 1; }
            else if (long.TryParse(sa, out a)) { b = sb == "" ? size - 1 : (long.TryParse(sb, out long bb) ? Math.Min(bb, size - 1) : -2); }
            else ok = false;
        }
        if (!ok || a < 0 || b < a || a >= size) { ctx.Response.StatusCode = 416; ctx.Response.Headers.ContentRange = $"bytes */{size}"; return; }
        start = a; end = b; status = 206; rangeText = $"{start}-{end}";
        ctx.Response.Headers.ContentRange = $"bytes {start}-{end}/{size}";
    }
    long length = end - start + 1;
    ctx.Response.StatusCode = status;
    ctx.Response.ContentType = "application/octet-stream";
    ctx.Response.ContentLength = length;
    ctx.Response.Headers.AcceptRanges = "bytes";
    long sent = 0;
    string kind = "locked";
    FastFile? ff = null;
    if (vol.Fast != null && ctx.Request.Method != "HEAD")
    {
        try { ff = vol.Fast.Get(path); }
        catch (Exception ex) { Console.WriteLine($"fast map failed {label}/{path}: {ex.Message} - locked path"); }
    }
    if (ff != null)
    {
        kind = "fast";
        byte[] buf = ArrayPool<byte>.Shared.Rent((int)Math.Min(length, 4L << 20));
        try
        {
            long pos = start;
            while (sent < length && !ctx.RequestAborted.IsCancellationRequested)
            {
                int want = (int)Math.Min(buf.Length, length - sent);
                vol.Fast!.Read(ff, pos, buf.AsSpan(0, want));
                await ctx.Response.Body.WriteAsync(buf.AsMemory(0, want), ctx.RequestAborted);
                pos += want; sent += want;
            }
        }
        finally { ArrayPool<byte>.Shared.Return(buf); }
    }
    else if (ctx.Request.Method != "HEAD")
    {
        using Stream inner = vol.V.OpenFile(path);
        byte[] buf = new byte[1 << 20];
        long pos = start;
        while (sent < length && !ctx.RequestAborted.IsCancellationRequested)
        {
            int want = (int)Math.Min(buf.Length, length - sent);
            int got;
            lock (vol.Lock) { inner.Position = pos; got = inner.Read(buf, 0, want); }   // the decoded image stream is shared: one reader at a time
            if (got <= 0) break;
            await ctx.Response.Body.WriteAsync(buf.AsMemory(0, got), ctx.RequestAborted);
            pos += got; sent += got;
        }
    }
    var reqMs = sw.ElapsedMilliseconds;
    if (status == 206) NoteLatency(reqMs);
    Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} {ctx.Request.Method} {label}/{path} range={rangeText} -> {status} {sent} B {reqMs} ms {kind}{(status == 206 && reqMs >= slowMs ? " SLOW" : "")}");
}

Console.WriteLine($"imgserve: {string.Join(" | ", roots)} on http://{bind}:{port} (read-only{(lockedOnly ? ", --locked: one reader per image" : ", fast parallel path for exFAT images")}), {catalog.All().Count} images, {catalog.All().Count(x => x.Tid != null)} with a title id");
app.Run();
return 0;

// Every image under the roots, by file name and by title id (see the header).
sealed class Catalog
{
    public sealed record Img(string Path, string Name, long Size, long Mtime, string? Tid, string? Version);
    readonly List<string> _roots;
    readonly int _depth;
    readonly string _cachePath;
    readonly object _lock = new();
    List<Img> _all = new();
    DateTime _lastScan = DateTime.MinValue;
    static readonly Regex TidInName = new(@"(?:\(|\[|^)(PPSA\d{5}|CUSA\d{5})(?:\)|\]|\s|_|-)", RegexOptions.Compiled);

    public Catalog(List<string> roots, int depth, string cachePath) { _roots = roots; _depth = depth; _cachePath = cachePath; }

    public List<Img> All() { lock (_lock) return _all; }

    public string? ByName(string name)
    {
        var hit = All().FirstOrDefault(x => x.Name.Equals(name, StringComparison.Ordinal) && File.Exists(x.Path));
        if (hit != null) return hit.Path;
        if (Scan("name " + name)) hit = All().FirstOrDefault(x => x.Name.Equals(name, StringComparison.Ordinal));
        return hit?.Path;
    }

    public string? ByTitle(string key, out string why)
    {
        string tid = key; long size = -1;
        int at = key.IndexOf('@');
        if (at > 0) { tid = key[..at]; if (!long.TryParse(key[(at + 1)..], out size)) { why = "bad @size"; return null; } }
        for (int attempt = 0; attempt < 2; attempt++)
        {
            var c = All().Where(x => string.Equals(x.Tid, tid, StringComparison.OrdinalIgnoreCase) && (size < 0 || x.Size == size) && File.Exists(x.Path)).ToList();
            if (c.Count == 1) { why = ""; return c[0].Path; }
            if (c.Count > 1 && c.All(x => x.Size == c[0].Size)) { why = ""; return c[0].Path; }   // the same image twice (a copy): either one
            if (c.Count > 1) { why = $"ambiguous: {c.Count} images of {tid} (add @<bytes>): " + string.Join(", ", c.Select(x => $"{x.Name}@{x.Size}")); return null; }
            if (attempt == 0 && !Scan("title " + key)) break;
        }
        why = "no image of " + key;
        return null;
    }

    // Returns true when a scan ran (at most every 15 s).
    public bool Scan(string why)
    {
        lock (_lock)
        {
            if ((DateTime.UtcNow - _lastScan).TotalSeconds < 15 && _all.Count > 0) return false;
            _lastScan = DateTime.UtcNow;
        }
        var sw = Stopwatch.StartNew();
        var cache = LoadCache();
        var found = new List<Img>();
        var opts = new EnumerationOptions { RecurseSubdirectories = true, MaxRecursionDepth = _depth, IgnoreInaccessible = true, AttributesToSkip = FileAttributes.System };
        foreach (var root in _roots)
        {
            IEnumerable<string> files;
            try { files = Directory.EnumerateFiles(root, "*.ffpfsc", opts).ToList(); } catch (Exception ex) { Console.WriteLine($"scan {root}: {ex.Message}"); continue; }
            foreach (var f in files)
            {
                var fi = new FileInfo(f);
                if (fi.Name.StartsWith("._")) continue;          // macOS AppleDouble files on exFAT
                long mt = new DateTimeOffset(fi.LastWriteTimeUtc).ToUnixTimeSeconds();
                string? tid = null, ver = null;
                if (cache.TryGetValue(f, out var c) && c.Size == fi.Length && c.Mtime == mt) { tid = c.Tid; ver = c.Version; }
                else (tid, ver) = ReadTitle(f);
                found.Add(new Img(f, fi.Name, fi.Length, mt, tid, ver));
            }
        }
        lock (_lock) _all = found;
        SaveCache(found);
        Console.WriteLine($"{DateTime.Now:HH:mm:ss} scan ({why}): {found.Count} images under {_roots.Count} root(s) in {sw.ElapsedMilliseconds} ms");
        return true;
    }

    static (string?, string?) ReadTitle(string path)
    {
        FfpfscVolume? v = null;
        try
        {
            v = FfpfscVolume.Open(path);
            var e = v.Entries.FirstOrDefault(x => !x.IsDirectory && x.Path.Replace('\\', '/').Trim('/').Equals("sce_sys/param.json", StringComparison.OrdinalIgnoreCase));
            if (e != null)
            {
                using var s = v.OpenFile(e.Path.Replace('\\', '/').Trim('/'));
                using var doc = JsonDocument.Parse(s);
                string? tid = doc.RootElement.TryGetProperty("titleId", out var t) ? t.GetString() : null;
                string? ver = doc.RootElement.TryGetProperty("contentVersion", out var cv) ? cv.GetString() : null;
                if (tid != null) return (tid, ver);
            }
        }
        catch (Exception ex) { Console.WriteLine($"title id of {System.IO.Path.GetFileName(path)}: {ex.Message} - using the file name"); }
        finally { (v as IDisposable)?.Dispose(); }
        var m = TidInName.Match(System.IO.Path.GetFileName(path));
        return (m.Success ? m.Groups[1].Value : null, null);
    }

    Dictionary<string, Img> LoadCache()
    {
        try
        {
            if (File.Exists(_cachePath))
                return (JsonSerializer.Deserialize<List<Img>>(File.ReadAllText(_cachePath)) ?? new()).ToDictionary(x => x.Path, x => x);
        }
        catch (Exception ex) { Console.WriteLine($"cache {_cachePath}: {ex.Message}"); }
        return new();
    }

    void SaveCache(List<Img> all)
    {
        try
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(_cachePath)!);
            File.WriteAllText(_cachePath + ".tmp", JsonSerializer.Serialize(all));
            File.Move(_cachePath + ".tmp", _cachePath, true);
        }
        catch (Exception ex) { Console.WriteLine($"cache save: {ex.Message}"); }
    }
}

sealed class Vol
{
    public readonly FfpfscVolume V;
    public readonly Dictionary<string, long> Sizes;
    public readonly object Lock = new();
    public readonly FastImage? Fast;
    public Vol(FfpfscVolume v, Dictionary<string, long> sizes, FastImage? fast) { V = v; Sizes = sizes; Fast = fast; }
}

// One inner file as runs of the decoded (logical) image: file bytes [FileOff[i], FileOff[i] + Len[i]) live at logical LogOff[i].
sealed class FastFile
{
    public required long Size;
    public required long[] FileOff;
    public required long[] LogOff;
    public required long[] Len;
}

// Lock-free reader for an exFAT image inside PFSC (see the FAST PATH note at the top). Thread-safe: no shared position, pread only.
sealed class FastImage
{
    readonly SafeFileHandle _h;
    readonly long _payload;            // container offset of the PFSC stream (FfpfscInfo.PayloadOffset)
    readonly long[] _blocks;           // PFSC block offsets (BlockCount + 1), relative to _payload
    readonly int _bs;                  // PFSC logical block size (64 KiB)
    readonly long _logicalLength;
    readonly ExfatVolume _fs;
    readonly Dictionary<string, PS5PKGTool.Ffpfsc.ExfatEntry> _entries = new(StringComparer.OrdinalIgnoreCase);
    readonly ConcurrentDictionary<string, Lazy<FastFile>> _files = new(StringComparer.OrdinalIgnoreCase);

    public FastImage(string path, FfpfscVolume v)
    {
        _fs = v.FileSystem;
        PfscInfo p = v.Info.Pfsc;
        _bs = p.LogicalBlockSize;
        if (_bs <= 0 || _bs > (1 << 22)) throw new InvalidDataException("odd PFSC block size");
        _blocks = p.BlockOffsets.ToArray();
        _payload = v.Info.PayloadOffset;
        _logicalLength = v.Info.LogicalLength;
        foreach (var e in _fs.Entries) if (!e.IsDirectory) _entries[e.Path.Replace('\\', '/').Trim('/')] = e;
        _h = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    }

    public FastFile? Get(string path)
    {
        if (!_entries.TryGetValue(path, out var e)) return null;
        return _files.GetOrAdd(path, _ => new Lazy<FastFile>(() => Build(e))).Value;
    }

    FastFile Build(PS5PKGTool.Ffpfsc.ExfatEntry e)
    {
        long cs = _fs.ClusterSize, heap = _fs.ClusterHeapOffset;
        var fo = new List<long>(); var lo = new List<long>(); var ln = new List<long>();
        if (e.Size > 0)
        {
            if (e.IsContiguous)
            {
                fo.Add(0); lo.Add(heap + ((long)e.FirstCluster - 2) * cs); ln.Add(e.Size);
            }
            else
            {
                IReadOnlyList<uint> cl = _fs.GetFileClusters(e.Path);       // FAT chain: walked once per file, then cached
                long fileOff = 0;
                for (int i = 0; i < cl.Count && fileOff < e.Size; i++)
                {
                    long logical = heap + ((long)cl[i] - 2) * cs;
                    long take = Math.Min(cs, e.Size - fileOff);
                    int last = lo.Count - 1;
                    if (last >= 0 && lo[last] + ln[last] == logical) ln[last] += take;
                    else { fo.Add(fileOff); lo.Add(logical); ln.Add(take); }
                    fileOff += take;
                }
                if (fileOff != e.Size) throw new InvalidDataException($"cluster chain of {e.Path} covers {fileOff} of {e.Size} bytes");
            }
        }
        for (int i = 0; i < lo.Count; i++)
            if (lo[i] < 0 || lo[i] + ln[i] > _logicalLength) throw new InvalidDataException($"{e.Path} run {i} leaves the image");
        return new FastFile { Size = e.Size, FileOff = fo.ToArray(), LogOff = lo.ToArray(), Len = ln.ToArray() };
    }

    // dst.Length bytes of the file at offset `off` (the caller keeps off + dst.Length <= Size)
    public void Read(FastFile f, long off, Span<byte> dst)
    {
        if (off < 0 || off + dst.Length > f.Size) throw new ArgumentOutOfRangeException(nameof(off));
        int done = 0;
        while (done < dst.Length)
        {
            long pos = off + done;
            int r = Array.BinarySearch(f.FileOff, pos);
            if (r < 0) r = ~r - 1;
            long inRun = pos - f.FileOff[r];
            int n = (int)Math.Min(dst.Length - done, f.Len[r] - inRun);
            if (n <= 0) throw new InvalidDataException("extent map gap");
            ReadLogical(f.LogOff[r] + inRun, dst.Slice(done, n));
            done += n;
        }
    }

    // decoded image bytes [L, L + dst.Length): one pread for the stored blocks, then inflate block by block
    void ReadLogical(long L, Span<byte> dst)
    {
        if (L < 0 || L + dst.Length > _logicalLength) throw new ArgumentOutOfRangeException(nameof(L));
        int b0 = (int)(L / _bs), b1 = (int)((L + dst.Length - 1) / _bs);
        long s0 = _blocks[b0], s1 = _blocks[b1 + 1];
        int storedLen = checked((int)(s1 - s0));
        byte[] stored = ArrayPool<byte>.Shared.Rent(storedLen);
        byte[] block = ArrayPool<byte>.Shared.Rent(_bs);
        try
        {
            int got = 0;
            while (got < storedLen)
            {
                int k = RandomAccess.Read(_h, stored.AsSpan(got, storedLen - got), _payload + s0 + got);
                if (k <= 0) throw new EndOfStreamException("image ends inside a PFSC block");
                got += k;
            }
            int done = 0;
            for (int b = b0; b <= b1; b++)
            {
                int so = (int)(_blocks[b] - s0), sl = (int)(_blocks[b + 1] - _blocks[b]);
                if (sl <= 0 || sl > _bs) throw new InvalidDataException($"PFSC block {b} has an invalid stored size");
                long bStart = (long)b * _bs;
                int from = (int)Math.Max(0, L - bStart);
                int to = (int)Math.Min(_bs, L + dst.Length - bStart);
                ReadOnlySpan<byte> plain;
                if (sl == _bs) plain = stored.AsSpan(so, _bs);
                else
                {
                    using var ms = new MemoryStream(stored, so, sl, writable: false);
                    using var z = new ZLibStream(ms, CompressionMode.Decompress);
                    int t = 0;
                    while (t < _bs) { int k = z.Read(block, t, _bs - t); if (k == 0) break; t += k; }
                    if (t != _bs) throw new InvalidDataException($"PFSC block {b} did not decode to {_bs} bytes");
                    plain = block.AsSpan(0, _bs);
                }
                plain.Slice(from, to - from).CopyTo(dst.Slice(done));
                done += to - from;
            }
            if (done != dst.Length) throw new InvalidDataException("decode length mismatch");
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(stored);
            ArrayPool<byte>.Shared.Return(block);
        }
    }
}
