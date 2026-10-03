// GPL-3.0-or-later. 仅新增软件自身分类中的歌曲，不含删除、覆盖或修改已有对象的入口。
// COM GUID、PROPERTYKEY 与方法顺序依据 Microsoft win32metadata 的
// PortableDeviceApi.h / PortableDeviceTypes.h / PortableDevice.h。
// https://learn.microsoft.com/windows/win32/api/portabledeviceapi/nf-portabledeviceapi-iportabledevicecontent-createobjectwithpropertiesanddata
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace Lmsc.Wpd {
    [StructLayout(LayoutKind.Sequential, Pack = 4)] internal struct PropertyKey {
        public Guid fmtid; public uint pid;
        public PropertyKey(string format, uint property) { fmtid = new Guid(format); pid = property; }
    }
    // PROPVARIANT 的值联合在 x64 为 16 字节，加 8 字节头，总计 24；只作 ABI 声明。
    [StructLayout(LayoutKind.Explicit, Size = 24)] internal struct PropVariant {
        [FieldOffset(0)] public ushort vt;
        [FieldOffset(8)] public IntPtr pointer;
        [FieldOffset(8)] public ulong unsignedValue;
        [FieldOffset(16)] public IntPtr secondPointer;
    }
    [ComImport, Guid("a1567595-4c2f-4574-a6fa-ecef917b9a40"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IDeviceManager {
        void GetDevices(IntPtr ids, ref uint count);
        void RefreshDeviceList();
        void GetDeviceFriendlyName([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr value, ref uint count);
    }
    [ComImport, Guid("625e2df8-6392-4cf0-9ad1-3cfa5f17775c"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IDevice {
        void Open([MarshalAs(UnmanagedType.LPWStr)] string id, IValues client);
        void SendCommand(uint flags, IValues input, out IValues output);
        void Content(out IContent content);
        void Capabilities(out IntPtr capabilities);
        void Cancel(); void Close();
    }
    [ComImport, Guid("6a96ed84-7c73-4480-9938-bf5af477d426"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IContent {
        void EnumObjects(uint flags, [MarshalAs(UnmanagedType.LPWStr)] string parent, IValues filter, out IObjectIds items);
        void Properties(out IProperties properties);
        void Transfer(out IResources resources);
        void CreateObjectWithPropertiesOnly(IValues values, out IntPtr objectId);
        void CreateObjectWithPropertiesAndData(IValues values, out IStream stream, ref uint bufferSize, out IntPtr cookie);
    }
    [ComImport, Guid("10ece955-cf41-4728-bfa0-41eedf1bbf19"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IObjectIds {
        [PreserveSig] int Next(uint count, IntPtr ids, ref uint fetched);
        void Skip(uint count); void Reset(); void Clone(out IObjectIds clone); void Cancel();
    }
    [ComImport, Guid("7f6d695c-03df-4439-a809-59266beee3a6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IProperties {
        void GetSupportedProperties([MarshalAs(UnmanagedType.LPWStr)] string id, out IntPtr keys);
        void GetPropertyAttributes([MarshalAs(UnmanagedType.LPWStr)] string id, ref PropertyKey key, out IValues values);
        void GetValues([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr keys, out IValues values);
    }
    [ComImport, Guid("fd8878ac-d841-4d17-891c-e6829cdb6934"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IResources {
        void GetSupportedResources([MarshalAs(UnmanagedType.LPWStr)] string id, out IntPtr keys);
        void GetResourceAttributes([MarshalAs(UnmanagedType.LPWStr)] string id, ref PropertyKey key, out IValues values);
        void GetStream([MarshalAs(UnmanagedType.LPWStr)] string id, ref PropertyKey key, uint mode, ref uint bufferSize, out IStream stream);
    }
    [ComImport, Guid("6848f6f2-3155-4f86-b6f5-263eeeab3143"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IValues {
        void GetCount(out uint count); void GetAt(uint index, ref PropertyKey key, ref PropVariant value);
        void SetValue(ref PropertyKey key, ref PropVariant value); void GetValue(ref PropertyKey key, out PropVariant value);
        void SetStringValue(ref PropertyKey key, [MarshalAs(UnmanagedType.LPWStr)] string value);
        [PreserveSig] int GetStringValue(ref PropertyKey key, out IntPtr value);
        void SetUnsignedIntegerValue(ref PropertyKey key, uint value); void GetUnsignedIntegerValue(ref PropertyKey key, out uint value);
        void SetSignedIntegerValue(ref PropertyKey key, int value); void GetSignedIntegerValue(ref PropertyKey key, out int value);
        void SetUnsignedLargeIntegerValue(ref PropertyKey key, ulong value);
        [PreserveSig] int GetUnsignedLargeIntegerValue(ref PropertyKey key, out ulong value);
        void SetSignedLargeIntegerValue(ref PropertyKey key, long value); void GetSignedLargeIntegerValue(ref PropertyKey key, out long value);
        void SetFloatValue(ref PropertyKey key, float value); void GetFloatValue(ref PropertyKey key, out float value);
        void SetErrorValue(ref PropertyKey key, int value); void GetErrorValue(ref PropertyKey key, out int value);
        void SetKeyValue(ref PropertyKey key, ref PropertyKey value); void GetKeyValue(ref PropertyKey key, out PropertyKey value);
        void SetBoolValue(ref PropertyKey key, [MarshalAs(UnmanagedType.Bool)] bool value);
        void GetBoolValue(ref PropertyKey key, [MarshalAs(UnmanagedType.Bool)] out bool value);
        void SetIUnknownValue(ref PropertyKey key, [MarshalAs(UnmanagedType.IUnknown)] object value);
        void GetIUnknownValue(ref PropertyKey key, [MarshalAs(UnmanagedType.IUnknown)] out object value);
        void SetGuidValue(ref PropertyKey key, ref Guid value);
        [PreserveSig] int GetGuidValue(ref PropertyKey key, out Guid value);
    }
    [ComImport, Guid("88e04db3-1012-4d64-9996-f703a950d3f4"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IDataStream {
        void Read([Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] byte[] buffer, int size, IntPtr read);
        void Write([MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] byte[] buffer, int size, IntPtr written);
        void Seek(long offset, int origin, IntPtr position); void SetSize(long size);
        void CopyTo(IStream target, long size, IntPtr read, IntPtr written);
        void Commit(int flags); void Revert(); void LockRegion(long offset, long size, int type);
        void UnlockRegion(long offset, long size, int type); void Stat(out System.Runtime.InteropServices.ComTypes.STATSTG stat, int flags); void Clone(out IStream clone);
        void GetObjectID(out IntPtr id); void Cancel();
    }
    internal static class Keys {
        const string Objects = "EF6B490D-5CD8-437A-AFFC-DA8B60EE4A3C";
        const string Clients = "204D9F0C-2292-4080-9F42-40664E70F859";
        public static PropertyKey Parent = new PropertyKey(Objects, 3), Name = new PropertyKey(Objects, 4),
            Persistent = new PropertyKey(Objects, 5), Format = new PropertyKey(Objects, 6), Type = new PropertyKey(Objects, 7),
            Size = new PropertyKey(Objects, 11), FileName = new PropertyKey(Objects, 12),
            Category = new PropertyKey("8F052D93-ABCA-4FC5-A5AC-B01DF4DBE598", 2),
            Resource = new PropertyKey("E81E79BE-34F0-41BF-B53F-F1A06AE87842", 0),
            ClientName = new PropertyKey(Clients, 2), Access = new PropertyKey(Clients, 9);
        public static Guid Folder = new Guid("27E2E392-A111-48E0-AB0C-E17705A05F85"),
            GenericFile = new Guid("0085E0A6-8D34-45D7-BC5C-447E59C73D48"),
            Storage = new Guid("23F05BBC-15DE-4C2A-A55B-A9AF5CE412EF"),
            FolderFormat = new Guid("30010000-AE6C-4804-98BA-C57B46965FE7"),
            FileFormat = new Guid("30000000-AE6C-4804-98BA-C57B46965FE7");
    }
    internal sealed class DeviceObject {
        public string Id, Persistent, Name, Parent; public Guid Type, Category;
        public bool IsFolder { get { return Type == Keys.Folder; } }
    }
    internal sealed class DeviceSession : IDisposable {
        IDevice device; IContent content; IProperties properties; IResources resources;
        readonly Action check;
        public DeviceSession(string id, bool write, Action cancelled) {
            check = cancelled;
            device = (IDevice)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("728a21c5-3d9e-48d7-9810-864848f0f404")));
            IValues values = NewValues();
            try {
                values.SetStringValue(ref Keys.ClientName, "光剑曲谱制作");
                values.SetUnsignedIntegerValue(ref Keys.Access, write ? 0xC0000000u : 0x80000000u);
                device.Open(id, values); device.Content(out content); content.Properties(out properties); content.Transfer(out resources);
            } catch { Dispose(); throw; } finally { Release(values); }
        }
        public static IValues NewValues() { return (IValues)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("0c15d503-d017-47ce-9016-7b3f978721cc"))); }
        public static void Release(object value) { if (value != null && Marshal.IsComObject(value)) Marshal.FinalReleaseComObject(value); }
        static string StringValue(IValues values, ref PropertyKey key) {
            IntPtr pointer = IntPtr.Zero;
            try { return values.GetStringValue(ref key, out pointer) >= 0 && pointer != IntPtr.Zero ? Marshal.PtrToStringUni(pointer) : ""; }
            finally { if (pointer != IntPtr.Zero) Marshal.FreeCoTaskMem(pointer); }
        }
        public DeviceObject Get(string id) {
            check(); IValues values; properties.GetValues(id, IntPtr.Zero, out values);
            try {
                var item = new DeviceObject { Id = id, Persistent = StringValue(values, ref Keys.Persistent),
                    Name = StringValue(values, ref Keys.FileName), Parent = StringValue(values, ref Keys.Parent) };
                if (item.Name.Length == 0) item.Name = StringValue(values, ref Keys.Name);
                values.GetGuidValue(ref Keys.Type, out item.Type); values.GetGuidValue(ref Keys.Category, out item.Category);
                return item;
            } finally { Release(values); }
        }
        public List<DeviceObject> Children(string parent) {
            check(); IObjectIds ids; content.EnumObjects(0, parent, null, out ids);
            IntPtr pointers = Marshal.AllocCoTaskMem(IntPtr.Size * 32);
            var list = new List<DeviceObject>();
            try {
                while (true) {
                    check(); for (int i = 0; i < 32; ++i) Marshal.WriteIntPtr(pointers, i * IntPtr.Size, IntPtr.Zero);
                    uint fetched = 0; int hr = ids.Next(32, pointers, ref fetched); if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                    if (fetched > 32) throw new IOException("设备对象枚举返回无效数量。");
                    for (int i = 0; i < (int)fetched; ++i) {
                        IntPtr id = Marshal.ReadIntPtr(pointers, i * IntPtr.Size);
                        try { list.Add(Get(Marshal.PtrToStringUni(id))); }
                        finally { if (id != IntPtr.Zero) Marshal.FreeCoTaskMem(id); Marshal.WriteIntPtr(pointers, i * IntPtr.Size, IntPtr.Zero); }
                        if (list.Count > 10000) throw new IOException("设备单层目录超过 10000 个对象，无法安全定位。");
                    }
                    if (fetched < 32) break;
                }
                return list;
            } finally {
                for (int i = 0; i < 32; ++i) { IntPtr id = Marshal.ReadIntPtr(pointers, i * IntPtr.Size); if (id != IntPtr.Zero) Marshal.FreeCoTaskMem(id); }
                Marshal.FreeCoTaskMem(pointers); Release(ids);
            }
        }
        public DeviceObject Child(string parent, string name) {
            var children = Children(parent).Where(item => String.Equals(item.Name, name, StringComparison.OrdinalIgnoreCase)).ToList();
            if (children.Count > 1) throw new IOException("设备含重复目录名称：" + name);
            return children.Count == 1 ? children[0] : null;
        }
        public DeviceObject ExistingRoot(string storage, string[] segments) {
            DeviceObject current = Get(storage);
            foreach (string segment in segments) { current = Child(current.Id, segment); if (current == null || !current.IsFolder) return null; }
            return current;
        }
        public string CreateFolder(string parent, string name) {
            if (!Program.SafeName(name)) throw new IOException("设备目录名不符合 Windows 安全命名规则。");
            check(); if (Child(parent, name) != null) throw new IOException("设备目录已存在，拒绝覆盖：" + name);
            IValues values = NewValues(); IntPtr pointer = IntPtr.Zero;
            try {
                values.SetStringValue(ref Keys.Parent, parent); values.SetStringValue(ref Keys.Name, name); values.SetStringValue(ref Keys.FileName, name);
                values.SetGuidValue(ref Keys.Type, ref Keys.Folder); values.SetGuidValue(ref Keys.Format, ref Keys.FolderFormat);
                content.CreateObjectWithPropertiesOnly(values, out pointer);
                string id = Marshal.PtrToStringUni(pointer); DeviceObject created = Get(id);
                if (!created.IsFolder || created.Parent != parent || !String.Equals(created.Name, name, StringComparison.Ordinal)) throw new IOException("设备未按请求创建新目录。");
                DeviceObject resolved = Child(parent, name);
                if (resolved == null || resolved.Id != id) throw new IOException("设备目录创建后出现重名，已停止上传。");
                return id;
            } finally { if (pointer != IntPtr.Zero) Marshal.FreeCoTaskMem(pointer); Release(values); }
        }
        public string WriteFile(string parent, string name, FileStream source, FileEntry entry, Action<long> progress) {
            check(); if (Child(parent, name) != null) throw new IOException("设备文件已存在，拒绝覆盖：" + name);
            IValues values = NewValues(); IStream stream = null; IntPtr cookie = IntPtr.Zero;
            IntPtr written = Marshal.AllocCoTaskMem(4); bool committed = false;
            try {
                values.SetStringValue(ref Keys.Parent, parent); values.SetStringValue(ref Keys.Name, name); values.SetStringValue(ref Keys.FileName, name);
                values.SetGuidValue(ref Keys.Type, ref Keys.GenericFile); values.SetGuidValue(ref Keys.Format, ref Keys.FileFormat);
                values.SetUnsignedLargeIntegerValue(ref Keys.Size, (ulong)entry.Size);
                uint optimal = 0; content.CreateObjectWithPropertiesAndData(values, out stream, ref optimal, out cookie);
                var data = (IDataStream)stream;
                byte[] buffer = new byte[optimal >= 4096 && optimal <= 1024 * 1024 ? (int)optimal : 65536];
                using (SHA256 hash = SHA256.Create()) {
                    long count = 0; int bytes; source.Position = 0;
                    while ((bytes = source.Read(buffer, 0, buffer.Length)) > 0) {
                        check(); Marshal.WriteInt32(written, 0); stream.Write(buffer, bytes, written);
                        if (Marshal.ReadInt32(written) != bytes) throw new IOException("设备未写入完整文件块。");
                        hash.TransformBlock(buffer, 0, bytes, null, 0); count += bytes; progress(bytes);
                    }
                    hash.TransformFinalBlock(new byte[0], 0, 0);
                    if (count != entry.Size || Hex(hash.Hash) != entry.Sha256) throw new IOException("上传过程中本地资源发生变化：" + entry.Path);
                }
                check(); stream.Commit(0); committed = true;
                IntPtr objectId = IntPtr.Zero;
                try { data.GetObjectID(out objectId); string id = Marshal.PtrToStringUni(objectId);
                    DeviceObject created = Get(id); DeviceObject resolved = Child(parent, name);
                    if (created.Parent != parent || resolved == null || resolved.Id != id || !String.Equals(created.Name, name, StringComparison.Ordinal)) throw new IOException("设备文件提交后定位不一致。");
                    return id;
                } finally { if (objectId != IntPtr.Zero) Marshal.FreeCoTaskMem(objectId); }
            } catch {
                if (stream != null && !committed) { try { stream.Revert(); } catch { } }
                throw;
            } finally { Release(stream); Release(values); Marshal.FreeCoTaskMem(written); if (cookie != IntPtr.Zero) Marshal.FreeCoTaskMem(cookie); }
        }
        public void Verify(string id, FileEntry entry) {
            check(); IStream stream; uint optimal = 0; resources.GetStream(id, ref Keys.Resource, 0, ref optimal, out stream);
            IntPtr read = Marshal.AllocCoTaskMem(4);
            try {
                byte[] buffer = new byte[optimal >= 4096 && optimal <= 1024 * 1024 ? (int)optimal : 65536];
                long count = 0;
                using (SHA256 hash = SHA256.Create()) {
                    while (true) {
                        check(); Marshal.WriteInt32(read, 0); stream.Read(buffer, buffer.Length, read);
                        int bytes = Marshal.ReadInt32(read); if (bytes < 0 || bytes > buffer.Length) throw new IOException("设备回读返回无效字节数。");
                        if (bytes == 0) break; count += bytes;
                        if (count > entry.Size) throw new IOException("设备回读资源大于预期：" + entry.Path);
                        hash.TransformBlock(buffer, 0, bytes, null, 0);
                    }
                    hash.TransformFinalBlock(new byte[0], 0, 0);
                    if (count != entry.Size || Hex(hash.Hash) != entry.Sha256) throw new IOException("设备回读 SHA256 校验失败：" + entry.Path);
                }
            } finally { Marshal.FreeCoTaskMem(read); Release(stream); }
        }
        public static string Hex(byte[] hash) { return BitConverter.ToString(hash).Replace("-", "").ToLowerInvariant(); }
        public void Dispose() {
            Release(resources); resources = null; Release(properties); properties = null; Release(content); content = null;
            if (device != null) { try { device.Close(); } catch { } Release(device); device = null; }
        }
    }
    internal sealed class FileEntry { public string Path, Sha256; public long Size; public FileStream Stream; }
    internal static class Program {
        static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024, RecursionLimit = 64 };
        static string cancelFile, location = "";
        static readonly Dictionary<string, string[]> Games = new Dictionary<string, string[]> {
            { "oasis", new[] { "SoulTopia", "BeatNote", "Custom" } },
            { "lightband", new[] { "Android", "data", "com.StarRiverVR.LightBand", "files", "CustomMusic" } }
        };
        static readonly HashSet<string> ReferenceKeys = new HashSet<string> { "_songFilename", "songFilename", "_coverImageFilename", "coverImageFilename", "_beatmapFilename", "beatmapFilename", "beatmapDataFilename", "lightshowDataFilename", "audioDataFilename" };
        static void Record(object record) { Console.WriteLine(Json.Serialize(record)); Console.Out.Flush(); }
        static void Check() { if (File.Exists(cancelFile)) throw new OperationCanceledException(); }
        static IDictionary<string, object> Object(object value) { var result = value as IDictionary<string, object>; if (result == null) throw new IOException("任务 JSON 对象无效。"); return result; }
        static string Text(IDictionary<string, object> value, string key) { object item; return value.TryGetValue(key, out item) && item is string ? (string)item : ""; }
        internal static bool SafeName(string name) {
            return !String.IsNullOrWhiteSpace(name) && name != "." && name != ".." && name.Length <= 240
                && name.IndexOfAny(Path.GetInvalidFileNameChars()) < 0 && !name.EndsWith(".") && !name.EndsWith(" ")
                && !Regex.IsMatch(name, @"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])($|\.)", RegexOptions.IgnoreCase);
        }
        static bool SafePath(string path) { return !String.IsNullOrEmpty(path) && path.IndexOf('\\') < 0 && !Path.IsPathRooted(path) && path.Split('/').All(SafeName); }
        static void NoLinks(string path) {
            for (var current = new DirectoryInfo(path); current != null; current = current.Parent)
                if ((current.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("拒绝链接或重解析目录：" + current.Name);
        }
        static List<string> LocalFiles(string root, string relative, int depth) {
            Check(); if (depth > 20) throw new IOException("歌曲资源超过 20 层。"); var files = new List<string>();
            foreach (string path in Directory.GetFileSystemEntries(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)))) {
                string name = Path.GetFileName(path); string item = relative.Length == 0 ? name : relative + "/" + name;
                if (!SafePath(item) || (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException("拒绝歌曲不安全资源路径：" + item);
                if (Directory.Exists(path)) files.AddRange(LocalFiles(root, item, depth + 1)); else files.Add(item);
                if (files.Count > 4000) throw new IOException("歌曲资源超过 4000 个文件。");
            }
            return files;
        }
        static List<string> References(object value, HashSet<string> names = null) {
            var result = new List<string>(); var map = value as IDictionary<string, object>;
            if (map != null) foreach (var item in map) {
                if (!ReferenceKeys.Contains(item.Key)) result.AddRange(References(item.Value, names));
                else {
                    string path = item.Value as string;
                    if (String.IsNullOrEmpty(path) && item.Key.IndexOf("coverImage", StringComparison.Ordinal) >= 0) continue;
                    if (names != null && item.Key.IndexOf("songFilename", StringComparison.Ordinal) >= 0
                        && path != null && path.EndsWith(".egg", StringComparison.OrdinalIgnoreCase) && !names.Contains(path)) {
                        string alternate = path.Substring(0, path.Length - 4) + ".ogg";
                        if (names.Contains(alternate)) path = alternate;
                        else {
                            var candidates = names.Where(name => name.IndexOf('/') < 0 && name.EndsWith(".ogg", StringComparison.OrdinalIgnoreCase)).ToList();
                            if (candidates.Count == 1) path = candidates[0];
                        }
                    }
                    if (!SafePath(path)) throw new IOException("Info.dat 中存在不安全引用。"); result.Add(path);
                }
            }
            else if (value is IEnumerable && !(value is string)) foreach (object item in (IEnumerable)value) result.AddRange(References(item, names));
            return result;
        }
        static List<FileEntry> Validate(string folder, IDictionary<string, object> job) {
            if (!Directory.Exists(folder) || !SafeName(Path.GetFileName(folder)) || !Regex.IsMatch(Path.GetFileName(folder), @"^.+-by光剑曲谱(?:-(?:[2-9]|[1-9][0-9]+))?$")
                || Path.GetFileName(Path.GetDirectoryName(folder)) != "光剑曲谱制作") throw new IOException("源目录必须是完整导出的 光剑曲谱制作/歌曲名-by光剑曲谱。不能上传工程 assets-* 目录。");
            NoLinks(folder);
            var actual = LocalFiles(folder, "", 0); var entries = new List<FileEntry>(); var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            try {
                object values; if (!job.TryGetValue("files", out values) || !(values is IEnumerable)) throw new IOException("任务缺少原资源清单。");
                long total = 0;
                foreach (object value in (IEnumerable)values) {
                    var item = Object(value); string path = Text(item, "path"), sha = Text(item, "sha256");
                    object sizeValue; if (!item.TryGetValue("size", out sizeValue)) throw new IOException("资源清单缺少大小。");
                    long size = Convert.ToInt64(sizeValue);
                    if (!SafePath(path) || Path.GetExtension(path).Equals(".lmsc", StringComparison.OrdinalIgnoreCase) || !Regex.IsMatch(sha, "^[a-f0-9]{64}$") || size < 0 || !names.Add(path)) throw new IOException("资源清单含非法条目。");
                    total += size; if (entries.Count >= 4000 || total > 4L * 1024 * 1024 * 1024) throw new IOException("资源清单超过上传限制。");
                    var entry = new FileEntry { Path = path, Size = size, Sha256 = sha };
                    // 保持所有句柄直到完成：FileShare.Read 阻止正常程序修改、删除或改名源文件。
                    entry.Stream = new FileStream(Path.Combine(folder, path.Replace('/', Path.DirectorySeparatorChar)), FileMode.Open, FileAccess.Read, FileShare.Read);
                    entries.Add(entry);
                    using (SHA256 hash = SHA256.Create()) if (entry.Stream.Length != size || DeviceSession.Hex(hash.ComputeHash(entry.Stream)) != sha) throw new IOException("源文件已发生变化：" + path);
                    entry.Stream.Position = 0;
                }
                if (actual.Count != names.Count || !new HashSet<string>(actual, StringComparer.OrdinalIgnoreCase).SetEquals(names)) throw new IOException("源文件清单已发生变化或含重名文件。");
                FileEntry info = entries.SingleOrDefault(item => item.Path.Equals("Info.dat", StringComparison.OrdinalIgnoreCase));
                if (info == null || info.Size > 16 * 1024 * 1024) throw new IOException("缺少 Info.dat。");
                object parsed; using (var reader = new StreamReader(info.Stream, Encoding.UTF8, true, 4096, true)) parsed = Json.DeserializeObject(reader.ReadToEnd()); info.Stream.Position = 0;
                var refs = References(Object(parsed), names);
                if (refs.Count == 0 || refs.Any(path => !names.Contains(path))) throw new IOException("Info.dat 引用的资源缺失。");
                string serialized = Json.Serialize(parsed);
                if (!(serialized.Contains("\"_songFilename\"") || serialized.Contains("\"songFilename\"")) || !(serialized.Contains("\"_beatmapFilename\"") || serialized.Contains("\"beatmapFilename\"") || serialized.Contains("\"beatmapDataFilename\""))) throw new IOException("Info.dat 缺少声音或谱面声明。");
                object declared; if (!job.TryGetValue("references", out declared) || !(declared is IEnumerable)) throw new IOException("任务缺少引用清单。");
                if (!new HashSet<string>(((IEnumerable)declared).Cast<object>().Select(value => value as string), StringComparer.OrdinalIgnoreCase).SetEquals(refs)) throw new IOException("Info.dat 引用与原清单不一致。");
                return entries;
            } catch { foreach (var entry in entries) entry.Stream.Dispose(); throw; }
        }
        static List<KeyValuePair<string, string>> Devices() {
            IDeviceManager manager = (IDeviceManager)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("0af10cec-2ecd-4b92-9581-34f6ae0637f3")));
            IntPtr pointers = IntPtr.Zero; var devices = new List<KeyValuePair<string, string>>(); uint allocated = 0;
            try {
                manager.RefreshDeviceList(); uint count = 0; manager.GetDevices(IntPtr.Zero, ref count);
                if (count > 128) throw new IOException("连接设备数量超过限制。"); if (count == 0) return devices;
                pointers = Marshal.AllocCoTaskMem(IntPtr.Size * (int)count); allocated = count;
                for (int i = 0; i < count; ++i) Marshal.WriteIntPtr(pointers, i * IntPtr.Size, IntPtr.Zero);
                manager.GetDevices(pointers, ref count); if (count > allocated) throw new IOException("设备列表在读取中发生变化。");
                for (int i = 0; i < count; ++i) {
                    Check(); string id = Marshal.PtrToStringUni(Marshal.ReadIntPtr(pointers, i * IntPtr.Size));
                    uint length = 0; manager.GetDeviceFriendlyName(id, IntPtr.Zero, ref length);
                    if (length > 32768) throw new IOException("设备名称长度超过限制。");
                    IntPtr text = Marshal.AllocCoTaskMem(((int)length + 1) * 2);
                    try { manager.GetDeviceFriendlyName(id, text, ref length); string name = Marshal.PtrToStringUni(text);
                        if (name.IndexOf("pico", StringComparison.OrdinalIgnoreCase) >= 0) devices.Add(new KeyValuePair<string, string>(id, name));
                    } finally { Marshal.FreeCoTaskMem(text); }
                }
                return devices;
            } finally {
                if (pointers != IntPtr.Zero) { for (int i = 0; i < allocated; ++i) { IntPtr id = Marshal.ReadIntPtr(pointers, i * IntPtr.Size); if (id != IntPtr.Zero) Marshal.FreeCoTaskMem(id); } Marshal.FreeCoTaskMem(pointers); }
                DeviceSession.Release(manager);
            }
        }
        static string GameName(string id) { return id == "oasis" ? "星穹绿洲" : "光之乐团"; }
        static void List() {
            var destinations = new List<object>();
            foreach (var device in Devices()) using (var session = new DeviceSession(device.Key, false, Check)) {
                foreach (var storage in session.Children("DEVICE").Where(item => item.Category == Keys.Storage)) {
                    if (storage.Persistent.Length == 0) throw new IOException("设备存储不提供持久标识，无法安全定位。");
                    foreach (var game in Games) {
                        var root = session.ExistingRoot(storage.Id, game.Value); if (root == null) continue;
                        if (root.Persistent.Length == 0) throw new IOException("游戏目录不提供持久标识，无法安全定位。");
                        destinations.Add(new { deviceId = device.Key, deviceName = device.Value, storageId = storage.Persistent, storageName = storage.Name,
                            rootId = root.Persistent, gameId = game.Key, gameName = GameName(game.Key) });
                    }
                }
            }
            Record(new { type = "destinations", destinations = destinations });
        }
        static void Upload(IDictionary<string, object> job) {
            string folder = Path.GetFullPath(Text(job, "localFolder")).TrimEnd(Path.DirectorySeparatorChar); var entries = Validate(folder, job);
            try {
                var requested = Object(job["destination"]); string deviceId = Text(requested, "deviceId"), gameId = Text(requested, "gameId");
                if (!Games.ContainsKey(gameId)) throw new IOException("目标游戏未在白名单中。");
                var device = Devices().SingleOrDefault(item => item.Key == deviceId);
                if (String.IsNullOrEmpty(device.Key)) throw new IOException("目标 PICO 未连接，请刷新设备。");
                using (var session = new DeviceSession(deviceId, true, Check)) {
                    var storages = session.Children("DEVICE").Where(item => item.Category == Keys.Storage && item.Persistent == Text(requested, "storageId")).ToList();
                    if (storages.Count != 1) throw new IOException("存储持久标识不匹配，请刷新设备。");
                    var storage = storages[0]; var root = session.ExistingRoot(storage.Id, Games[gameId]);
                    if (root == null || root.Persistent != Text(requested, "rootId")) throw new IOException("游戏根目录缺失或标识变化，不创建游戏根目录。");
                    string categoryId; var category = session.Child(root.Id, "光剑曲谱制作");
                    if (category == null) categoryId = session.CreateFolder(root.Id, "光剑曲谱制作");
                    else if (category.IsFolder) categoryId = category.Id;
                    else throw new IOException("游戏分类名称已被文件占用。");
                    string original = Path.GetFileName(folder), songName = original;
                    string baseName = Regex.Replace(original, @"(-by光剑曲谱)-(?:[2-9]|[1-9][0-9]+)$", "$1");
                    int suffix = 2;
                    while (session.Child(categoryId, songName) != null) { songName = baseName + "-" + suffix++; if (suffix > 10000) throw new IOException("同名歌曲数量超过限制。"); }
                    location = device.Value + "\\" + storage.Name + "\\" + String.Join("\\", Games[gameId]) + "\\光剑曲谱制作\\" + songName;
                    // 在创建前记录可能的残留位置；即使驱动创建后断连，界面仍能给出检查位置。
                    Record(new { type = "created", location = location }); string songId = session.CreateFolder(categoryId, songName);
                    var folders = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { { "", songId } };
                    var uploaded = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    long total = entries.Sum(item => item.Size), sent = 0;
                    foreach (var entry in entries.OrderBy(item => item.Path.Equals("Info.dat", StringComparison.OrdinalIgnoreCase) ? 1 : 0).ThenBy(item => item.Path, StringComparer.Ordinal)) {
                        Check(); string parentPath = entry.Path.IndexOf('/') < 0 ? "" : entry.Path.Substring(0, entry.Path.LastIndexOf('/'));
                        string currentPath = "", parent = songId;
                        foreach (string segment in parentPath.Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries)) {
                            string next = currentPath.Length == 0 ? segment : currentPath + "/" + segment;
                            if (!folders.ContainsKey(next)) folders[next] = session.CreateFolder(parent, segment);
                            parent = folders[next]; currentPath = next;
                        }
                        if (entry.Path.Equals("Info.dat", StringComparison.OrdinalIgnoreCase)) {
                            if (LocalFiles(folder, "", 0).Count != entries.Count) throw new IOException("上传前源资源清单发生变化。");
                            foreach (string reference in References(Json.DeserializeObject(File.ReadAllText(Path.Combine(folder, entry.Path), Encoding.UTF8)), new HashSet<string>(entries.Select(item => item.Path), StringComparer.OrdinalIgnoreCase)))
                                if (!uploaded.ContainsKey(reference)) throw new IOException("Info.dat 发布前资源尚未验证：" + reference);
                        }
                        string id = session.WriteFile(parent, Path.GetFileName(entry.Path), entry.Stream, entry, delegate(long bytes) {
                            sent += bytes; Record(new { type = "progress", message = "正在上传并回读校验：" + entry.Path, percent = total == 0 ? 0 : (int)Math.Min(99, sent * 100 / total) });
                        });
                        session.Verify(id, entry); uploaded.Add(entry.Path, id);
                    }
                    if (uploaded.Count != entries.Count) throw new IOException("设备资源校验不完整。");
                    foreach (var entry in entries) { Check(); session.Get(uploaded[entry.Path]); }
                    Check(); Record(new { type = "uploaded", localFolder = folder, location = location, verified = true });
                }
            } finally { foreach (var entry in entries) entry.Stream.Dispose(); }
        }
        [STAThread] static int Main(string[] args) {
            Console.OutputEncoding = new UTF8Encoding(false);
            try {
                if (IntPtr.Size != 8) throw new IOException("设备导出工具需要 64 位 Windows。");
                if (args.Length != 2 || args[0] != "--job") throw new IOException("请由光剑曲谱制作使用任务文件调用设备导出工具。");
                string jobPath = Path.GetFullPath(args[1]); var task = new FileInfo(jobPath);
                if (!task.Exists || task.Length > 16 * 1024 * 1024) throw new IOException("任务文件缺失或超过限制。");
                var job = Object(Json.DeserializeObject(File.ReadAllText(jobPath, Encoding.UTF8)));
                if (Convert.ToInt32(job["protocol"]) != 1) throw new IOException("不支持的设备导出协议。");
                cancelFile = Path.GetFullPath(Text(job, "cancelFile"));
                if (!String.Equals(Path.GetDirectoryName(cancelFile), Path.GetDirectoryName(jobPath), StringComparison.OrdinalIgnoreCase)) throw new IOException("取消标志必须位于任务目录。");
                Check(); string mode = Text(job, "mode");
                if (mode == "list") List();
                else if (mode == "upload") Upload(job);
                else if (mode == "validate") {
                    string folder = Path.GetFullPath(Text(job, "localFolder")).TrimEnd(Path.DirectorySeparatorChar);
                    var entries = Validate(folder, job);
                    try { Record(new { type = "validated", files = entries.Count }); }
                    finally { foreach (var entry in entries) entry.Stream.Dispose(); }
                } else throw new IOException("未知设备导出任务。"); return 0;
            } catch (OperationCanceledException) { Record(new { type = "cancelled", location = location }); return 2; }
            catch (Exception error) { Record(new { type = "error", message = "设备导出失败：" + error.Message, location = location }); return 1; }
        }
    }
}
