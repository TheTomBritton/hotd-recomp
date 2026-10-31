/*
 * wasmfs_fetch.js - Emscripten's WASMFS fetch backend (src/lib/libwasmfs_fetch.js,
 * MIT, The Emscripten Authors), linked after it with --js-library so these
 * definitions replace it, with two fixes a real web host turned up:
 *
 * 1. The size probe is a HEAD with "Range: bytes=0-". LiteSpeed (and others)
 *    answer it 206 with the total in Content-Range and no Content-Length, which
 *    the original took as "no ranges here" and downloaded the whole file. The
 *    total is read from Content-Range too.
 * 2. After that whole-file fallback the file's chunk size is its own length,
 *    but later range arithmetic used the backend's chunk size: every read past
 *    the first megabyte fetched a range and stored it as a chunk the read then
 *    ignored, so the game's import table (at the end of its program) read as
 *    zeroes. The arithmetic now uses the file's recorded chunk size.
 */
/**
 * @license
 * Copyright 2022 The Emscripten Authors
 * SPDX-License-Identifier: MIT
 */

addToLibrary({
  $wasmFS$JSMemoryRanges: {},

  // Fetch backend: On first access of the file (either a read or a getSize), it
  // will fetch() the data from the network asynchronously. Otherwise, after
  // that fetch it behaves just like JSFile (and it reuses the code from there).

  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends',
    '$wasmFS$JSMemoryRanges',
    '_wasmfs_fetch_get_file_url',
    '_wasmfs_fetch_get_chunk_size',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    // Get a promise that fetches the data and stores it in JS memory (if it has
    // not already been fetched).
    async function getFileRange(file, offset, len) {
      var url = '';
      var fileUrl_p = __wasmfs_fetch_get_file_url(file);
      var fileUrl = UTF8ToString(fileUrl_p);
      var isAbs = fileUrl.indexOf('://') !== -1;
      if (isAbs) {
        url = fileUrl;
      } else {
        try {
          var u = new URL(fileUrl, self.location.origin);
          url = u.toString();
        } catch (_e) {
          throw {status: 404};
        }
      }
      var chunkSize = __wasmfs_fetch_get_chunk_size(file);
      offset ??= 0;
      len ??= chunkSize;
      // In which chunk does the seeked range start?  E.g., 5-14 with chunksize 8 will start in chunk 0.
      if (!(file in wasmFS$JSMemoryRanges)) {
        var fileInfo = await fetch(url, {method:'HEAD', headers:{'Range': 'bytes=0-'}});
        // the total: Content-Length, or the total in a 206's Content-Range ("bytes 0-N/TOTAL")
        var total = NaN;
        if (fileInfo.ok) {
          if (fileInfo.headers.has('Content-Length')) total = parseInt(fileInfo.headers.get('Content-Length'), 10);
          var cr = fileInfo.headers.get('Content-Range');
          if (cr && /\/(\d+)\s*$/.test(cr)) total = parseInt(/\/(\d+)\s*$/.exec(cr)[1], 10);
        }
        if (fileInfo.ok &&
            !isNaN(total) &&
            (fileInfo.headers.get('Accept-Ranges') == 'bytes' || fileInfo.status == 206) &&
            (total > chunkSize*2)) {
          var size = total;
          wasmFS$JSMemoryRanges[file] = {
            size,
            chunks: [],
            chunkSize: chunkSize
          };
          len = Math.min(len, size-offset);
        } else {
          // may as well/forced to download the whole file
          var wholeFileReq = await fetch(url);
          if (!wholeFileReq.ok) {
            throw wholeFileReq;
          }
          var wholeFileData = new Uint8Array(await wholeFileReq.arrayBuffer());
          wasmFS$JSMemoryRanges[file] = {
            size: wholeFileData.byteLength,
            chunks: [wholeFileData],
            chunkSize: wholeFileData.byteLength
          };
          return;
        }
      }
      chunkSize = wasmFS$JSMemoryRanges[file].chunkSize;   // the file's own (its length after a whole-file download)
      var firstChunk = (offset / chunkSize) | 0;
      // In which chunk does the seeked range end?  E.g., 5-14 with chunksize 8 will end in chunk 1, as will 5-16 (since byte 16 isn't requested).
      // This will always give us a chunk >= firstChunk since len > 0.
      var lastChunk = ((offset+len-1) / chunkSize) | 0;
      var allPresent = true;
      var i;
      // Do we have all the chunks already?  If so, we don't need to do any fetches.
      for (i = firstChunk; i <= lastChunk; i++) {
        if (!wasmFS$JSMemoryRanges[file].chunks[i]) {
          allPresent = false;
          break;
        }
      }
      if (allPresent) {
        // The data is already here, so nothing to do before we continue on to
        // the actual read.
        return;
      }
      // This is the first time we want the chunks' data.  We'll make
      // one request for all the chunks we need, rather than one
      // request per chunk.
      var start = firstChunk * chunkSize;
      // We must fetch *up to* the last byte of the last chunk.
      var end = (lastChunk+1) * chunkSize;
      var response = await fetch(url, {headers:{'Range': `bytes=${start}-${end-1}`}});
      if (!response.ok) {
        throw response;
      }
#if MIN_FIREFOX_VERSION < 128 || MIN_CHROME_VERSION < 132 || MIN_SAFARI_VERSION < 180000 || MIN_NODE_VERSION < 220300
      // Use the old .arrayBuffer() method when targeting old environments.
      var bytes = new Uint8Array(await response['arrayBuffer']());
#else
      // Use the new .bytes() method to save a bit of code size when all target environments have it. https://developer.mozilla.org/en-US/docs/Web/API/Response/bytes
      var bytes = await response['bytes']();
#endif
      for (i = firstChunk; i <= lastChunk; i++) {
        wasmFS$JSMemoryRanges[file].chunks[i] = bytes.slice(i*chunkSize-start,(i+1)*chunkSize-start);
      }
    }

    wasmFS$backends[backend] = {
      // alloc/free operations are not actually async. Just forward to the
      // parent class, but we must return a Promise as the caller expects.
      allocFile: async (file) => { /* nop */ },
      freeFile: async (file) => {
        // free memory
        wasmFS$JSMemoryRanges[file] = undefined;
      },

      write: async (file, buffer, length, offset) => {
        console.error('TODO: file writing in fetch backend? read-only for now');
      },

      // read/getSize fetch the data, then forward to the parent class.
      read: async (file, buffer, length, offset) => {
        // This function assumes that offset is non-negative and length is positive.
        // C read() doesn't take an offset and so doesn't have to deal with the former situation,
        // and if the length is 0 or the offset is negative there's no reasonable read we can make.
        if (offset < 0 || length <= 0) {
          return 0;
        }
        try {
          await getFileRange(file, offset || 0, length);
        } catch (failedResponse) {
          return failedResponse.status === 404 ? -{{{ cDefs.ENOENT }}} : -{{{ cDefs.EBADF }}};
        }
        var fileInfo = wasmFS$JSMemoryRanges[file];
        length = Math.min(length, fileInfo.size-offset);
        // As above, we check the length just in case offset was beyond size and length is now negative.
        if (length <= 0) {
          return 0;
        }
        var chunks = fileInfo.chunks;
        var chunkSize = fileInfo.chunkSize;
        var firstChunk = (offset / chunkSize) | 0;
        // See comments in getFileRange.
        var lastChunk = ((offset+length-1) / chunkSize) | 0;
        var readLength = 0;
        for (var i = firstChunk; i <= lastChunk; i++) {
          var chunk = chunks[i];
          var start = Math.max(i*chunkSize, offset);
          var chunkStart = i*chunkSize;
          var end = Math.min(chunkStart+chunkSize, offset+length);
          HEAPU8.set(chunk.subarray(start-chunkStart, end-chunkStart), buffer+(start-offset));
          readLength = end - offset;
        }
        return readLength;
      },
      getSize: async (file) => {
        try {
          await getFileRange(file, 0, 0);
        } catch (failedResponse) {
          return 0;
        }
        return wasmFS$JSMemoryRanges[file].size;
      },
    };
  },

});
