# NTFS Indexer

The first open source NTFS indexer that works on Linux and Windows. It reads the NTFS partition as a block device (or an exported raw `$MFT` binary file) and parses the MFT structure to index every single file/folder.

## Build

The project dependencies are managed by **Conan 2** and built with **CMake (>= 3.23)**.

1. **Install dependencies**:
   ```bash
   conan install . --build=missing -pr msvc-debug -c tools.cmake.cmaketoolchain:generator=Ninja
   ```
2. **Configure with CMake**:
   ```bash
   # Load Conan environment and configure
   build\Debug\generators\conanbuild.bat
   cmake --preset conan-debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
   ```
3. **Build**:
   ```bash
   cmake --build --preset conan-debug
   ```
## Running the Server

The server can be configured in three ways (ordered by increasing priority):
1. **Defaults**: Hardcoded fallbacks (`$MFT`, `8080`, `0.0.0.0`, `./web`).
2. **Configuration File**: Settings loaded from a flag file using Abseil's native `--flagfile` parameter.
3. **Command-line Flags**: Override settings via explicit Abseil flags.

### Configuration File (`flags.txt`)
Instead of long command-line options, you can use Abseil's built-in flagfile mechanism.
Create a file, e.g., `flags.txt`:
```txt
--device_path=$MFT
--port=8080
--address=0.0.0.0
--doc_root=./web
```

### Starting the Server

**Option A: Using defaults** (reads options directly from flag definitions):
```bash
build\Debug\ntfsindex.exe
```

**Option B: Using Abseil command-line flags** (highest priority override):
```bash
build\Debug\ntfsindex.exe --device_path \\.\C: --port 9000 --address 127.0.0.1 --doc_root .\web
```

**Option C: Using a flag file** (loads flags from a file):
```bash
build\Debug\ntfsindex.exe --flagfile=flags.txt
```

To view all available command-line configuration options, run the executable with `--helpfull`:
```bash
build\Debug\ntfsindex.exe --helpfull
```

## HTTP API Endpoints

The C++ backend exposes the following RESTful API endpoints:

### 1. Retrieve Scan Statistics
- **Endpoint**: `GET /api/stats`
- **Response**:
  ```json
  {
    "device_path": "\\\\.\\C:",
    "total_directories": 12435,
    "total_files": 89432,
    "total_logical_size": 2489032049,
    "total_logical_size_formatted": "2.32 GB"
  }
  ```

### 2. Fast File / Folder Search
- **Endpoint**: `GET /api/search?q=<query_string>&limit=100`
- **Response**:
  ```json
  {
    "total_matches": 12,
    "limit": 100,
    "results": [
      {
        "id": 12345,
        "parent_id": 5,
        "name": "important_document.pdf",
        "is_directory": false,
        "size": 1048576,
        "full_path": "/Documents/important_document.pdf"
      }
    ]
  }
  ```

### 3. Read USN Change Journal Stream
- **Endpoint**: `GET /api/usn?limit=100`
- **Response**:
  ```json
  {
    "success": true,
    "total_records": 1050,
    "entries": [
      {
        "usn": "0x0000000003F4A810",
        "file_id": 89432,
        "parent_id": 5,
        "filename": "temp_file.tmp",
        "reason": "CREATE|DATA_EXTEND|CLOSE",
        "timestamp": "2026-06-20 16:00:50 UTC"
      }
    ]
  }
  ```

### 4. Trigger Incremental Index Scan (USN Journal update)
- **Endpoint**: `POST /api/update`
- **Response**:
  ```json
  {
    "success": true,
    "elapsed_ms": 15,
    "last_usn": "0x3F4A810"
  }
  ```

## LICENSE

GPLv3