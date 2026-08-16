<#
.SYNOPSIS
Image helpers shared by the live capture harness.

.DESCRIPTION
The mirror writes a binary PPM because that is what the renderer can emit
without linking an image library into a plugin the game loads. Turning it into
a PNG happens here so the shipped module keeps no such dependency.

The PNG is written by hand rather than through System.Drawing. That assembly
is a Windows-only compatibility shim in PowerShell 7 and does not resolve for
compiled helpers here, and a capture run is far too expensive to lose to a
missing assembly. A truecolor PNG is a signature, three chunks, and a zlib
stream, all of which the base library already provides.

The pixel work is compiled rather than written in PowerShell. A per-byte loop
over a 1280x720 frame is both slow and easy to get silently wrong: the first
version of this returned a fully black image and still reported success,
because nothing about a wrong pixel copy raises an error.
#>

if (-not ('Vf.Image' -as [type])) {
    Add-Type -Namespace 'Vf' -Name 'Image' -MemberDefinition @'
private static uint[] crcTable;

private static uint Crc32(byte[] data, int offset, int count, uint seed)
{
    if (crcTable == null) {
        crcTable = new uint[256];
        for (uint n = 0; n < 256; n++) {
            uint c = n;
            for (int k = 0; k < 8; k++) {
                c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            crcTable[n] = c;
        }
    }
    uint crc = seed;
    for (int i = 0; i < count; i++) {
        crc = crcTable[(crc ^ data[offset + i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

private static void BeInt(System.IO.Stream stream, int value)
{
    stream.WriteByte((byte)(value >> 24));
    stream.WriteByte((byte)(value >> 16));
    stream.WriteByte((byte)(value >> 8));
    stream.WriteByte((byte)value);
}

private static void Chunk(System.IO.Stream stream, string type, byte[] data)
{
    BeInt(stream, data.Length);
    byte[] name = System.Text.Encoding.ASCII.GetBytes(type);
    stream.Write(name, 0, name.Length);
    stream.Write(data, 0, data.Length);
    uint crc = Crc32(name, 0, name.Length, 0xFFFFFFFFu);
    crc = Crc32(data, 0, data.Length, crc) ^ 0xFFFFFFFFu;
    BeInt(stream, unchecked((int)crc));
}

// Parses a binary PPM (P6) and writes it as a truecolor PNG. Returns false
// rather than throwing so a capture run is never lost to an unreadable dump.
public static bool PpmToPng(string source, string destination)
{
    byte[] bytes = System.IO.File.ReadAllBytes(source);
    int cursor = 0;
    string[] fields = new string[4];
    for (int field = 0; field < 4; field++) {
        while (cursor < bytes.Length && char.IsWhiteSpace((char)bytes[cursor])) cursor++;
        // A comment runs to the end of its line and may appear between fields.
        while (cursor < bytes.Length && bytes[cursor] == (byte)'#') {
            while (cursor < bytes.Length && bytes[cursor] != (byte)'\n') cursor++;
            while (cursor < bytes.Length && char.IsWhiteSpace((char)bytes[cursor])) cursor++;
        }
        int start = cursor;
        while (cursor < bytes.Length && !char.IsWhiteSpace((char)bytes[cursor])) cursor++;
        if (cursor <= start) return false;
        fields[field] = System.Text.Encoding.ASCII.GetString(bytes, start, cursor - start);
    }
    // Exactly one whitespace byte separates the header from the payload.
    cursor++;
    if (fields[0] != "P6") return false;
    int width, height, maximum;
    if (!int.TryParse(fields[1], out width) ||
        !int.TryParse(fields[2], out height) ||
        !int.TryParse(fields[3], out maximum)) return false;
    if (width <= 0 || height <= 0 || maximum != 255) return false;
    long needed = (long)width * height * 3;
    if (bytes.Length - cursor < needed) return false;

    // Every scanline carries a leading filter byte. Filter 0 is "none",
    // which keeps this honest: the bytes in the file are the bytes the
    // renderer produced, with no prediction to get wrong.
    byte[] raw = new byte[(long)height * (width * 3 + 1)];
    for (int y = 0; y < height; y++) {
        long write = (long)y * (width * 3 + 1);
        raw[write] = 0;
        System.Array.Copy(bytes, cursor + (long)y * width * 3, raw, write + 1, width * 3);
    }

    byte[] compressed;
    using (var buffer = new System.IO.MemoryStream()) {
        using (var deflate = new System.IO.Compression.ZLibStream(
                buffer, System.IO.Compression.CompressionLevel.Optimal, true)) {
            deflate.Write(raw, 0, raw.Length);
        }
        compressed = buffer.ToArray();
    }

    using (var file = System.IO.File.Create(destination)) {
        byte[] signature = new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 };
        file.Write(signature, 0, signature.Length);
        byte[] header = new byte[13];
        header[0] = (byte)(width >> 24); header[1] = (byte)(width >> 16);
        header[2] = (byte)(width >> 8);  header[3] = (byte)width;
        header[4] = (byte)(height >> 24); header[5] = (byte)(height >> 16);
        header[6] = (byte)(height >> 8);  header[7] = (byte)height;
        header[8] = 8;   // bits per channel
        header[9] = 2;   // truecolor RGB
        header[10] = 0;  // deflate
        header[11] = 0;  // adaptive filtering
        header[12] = 0;  // no interlace
        Chunk(file, "IHDR", header);
        Chunk(file, "IDAT", compressed);
        Chunk(file, "IEND", new byte[0]);
    }
    return true;
}
'@ | Out-Null
}

function Convert-VfPpmToPng([string]$Source, [string]$Destination)
{
    try {
        if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { return $false }
        if (-not [Vf.Image]::PpmToPng(
                [System.IO.Path]::GetFullPath($Source),
                [System.IO.Path]::GetFullPath($Destination))) {
            return $false
        }
        return (Test-Path -LiteralPath $Destination -PathType Leaf)
    } catch {
        return $false
    }
}
