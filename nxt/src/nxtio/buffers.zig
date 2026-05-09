// Design sketch for nxt buffered I/O.
//
// This file is intentionally not part of the Meson build.  It uses Zig-ish
// syntax because the design notes came from reading Zig's std.Io.Reader,
// std.Io.Writer, and std.tar, but the target implementation for nxt is still
// C++ coroutine code under nxt/src/nxtio.
//
// The important idea is that the buffer is part of the stream interface, not
// hidden below it.  Most callers should provide storage explicitly, usually as
// stack arrays in the coroutine near the top of the application/protocol stage:
//
//     var read_buf: [16 * 1024]u8 = undefined;
//     var write_buf: [16 * 1024]u8 = undefined;
//     var reader = Reader.init(transport_source, &read_buf, cancel);
//     var writer = Writer.init(transport_sink, &write_buf, cancel);
//
// That keeps buffer-size policy in the application domain.  Some sizes are
// protocol-significant: TLS records, terminal frame batching, HTTP head limits,
// JSON/SSE line limits, and application latency goals can all justify different
// choices.  Buffering decisions are also scheduling decisions because each
// fill, drain, flush, and coroutine suspension changes which work can run next.

pub const Cancellation = struct {
    // Placeholder shape.  The C++ implementation might use stop_token,
    // libcoro cancellation, an intrusive operation handle, or an nxt-specific
    // cancellation source.  The point is that blocking/suspending operations
    // need a cancellation path at the same level as read/write scheduling.
    requested: bool = false,

    pub fn throwIfRequested(self: *Cancellation) !void {
        if (self.requested) return error.Cancelled;
    }
};

pub const Reader = struct {
    // The transport/source implementation.  In C++ this might be a template
    // parameter at first, and maybe type-erased later.
    source: *anyopaque,
    vtable: *const VTable,

    // Borrowed storage.  The reader never decides this allocation policy.
    buffer: []u8,

    // Buffered readable bytes are buffer[seek..end].
    // Bytes before seek have been consumed.  Bytes after end are undefined.
    seek: usize = 0,
    end: usize = 0,

    cancel: *Cancellation,

    pub const VTable = struct {
        // Fill unused reader capacity from the source.
        //
        // Returning zero should not itself mean EOF unless the operation has a
        // separate EOF result.  Short reads are allowed and often simplify the
        // implementation, especially for TLS and decompression-like sources.
        readSome: *const fn (r: *Reader, dst: []u8) anyerror!ReadResult,

        // Optional optimized pump from this reader into a writer.  The default
        // can repeatedly expose buffered data and call readSome.
        stream: ?*const fn (r: *Reader, w: *Writer, limit: Limit) anyerror!usize = null,

        // Optional optimized discard.  Useful for protocol padding, skipped
        // body bytes, tar-style unread file content, and ignored SSE fields.
        discard: ?*const fn (r: *Reader, limit: Limit) anyerror!usize = null,

        // Make it possible to buffer at least capacity bytes from the current
        // logical position.  The default is to memmove buffer[seek..end] to the
        // front.  Custom implementations can resize, rotate, or otherwise
        // preserve source-specific state.
        rebase: ?*const fn (r: *Reader, capacity: usize) anyerror!void = null,
    };

    pub fn buffered(self: *Reader) []u8 {
        return self.buffer[self.seek..self.end];
    }

    pub fn bufferedLen(self: *Reader) usize {
        return self.end - self.seek;
    }

    pub fn unusedCapacity(self: *Reader) []u8 {
        return self.buffer[self.end..];
    }

    pub fn rebase(self: *Reader, capacity: usize) !void {
        if (self.buffer.len - self.seek >= capacity) return;
        if (self.vtable.rebase) |f| return f(self, capacity);

        const pending = self.buffer[self.seek..self.end];
        if (capacity > self.buffer.len) return error.BufferTooSmall;
        @memmove(self.buffer[0..pending.len], pending);
        self.seek = 0;
        self.end = pending.len;
    }

    pub fn fill(self: *Reader, n: usize) !void {
        if (self.bufferedLen() >= n) return;
        try self.rebase(n);

        while (self.bufferedLen() < n) {
            try self.cancel.throwIfRequested();
            const result = try self.vtable.readSome(self, self.unusedCapacity());
            switch (result) {
                .bytes => |count| {
                    if (count == 0) return error.ProgressRequired;
                    self.end += count;
                },
                .eof => return error.EndOfStream,
            }
        }
    }

    pub fn fillMore(self: *Reader) !void {
        try self.rebase(self.bufferedLen() + 1);
        try self.cancel.throwIfRequested();
        const result = try self.vtable.readSome(self, self.unusedCapacity());
        switch (result) {
            .bytes => |count| {
                if (count == 0) return error.ProgressRequired;
                self.end += count;
            },
            .eof => return error.EndOfStream,
        }
    }

    // Borrowed view.  Valid until the next operation that can refill or rebase.
    pub fn peek(self: *Reader, n: usize) ![]u8 {
        try self.fill(n);
        return self.buffer[self.seek .. self.seek + n];
    }

    pub fn toss(self: *Reader, n: usize) void {
        // In the C++ API this should probably assert or throw on misuse.
        self.seek += n;
    }

    // Borrowed view plus commit.  Valid until the next refill/rebase.
    pub fn take(self: *Reader, n: usize) ![]u8 {
        const out = try self.peek(n);
        self.toss(n);
        return out;
    }

    pub fn takeUntil(self: *Reader, delimiter: []const u8) ![]u8 {
        while (true) {
            if (indexOf(self.buffered(), delimiter)) |i| {
                const out = self.buffer[self.seek .. self.seek + i];
                self.seek += i + delimiter.len;
                return out;
            }

            // Capacity is a protocol limit here.  If the delimiter does not
            // fit in the borrowed buffer, the parser should report that the
            // current unit is too large rather than silently allocating.
            if (self.end == self.buffer.len) return error.StreamTooLong;
            try self.fillMore();
        }
    }

    pub fn streamExact(self: *Reader, writer: *Writer, n: usize) !void {
        var remaining = n;
        while (remaining > 0) {
            const chunk = self.buffered();
            if (chunk.len != 0) {
                const count = @min(chunk.len, remaining);
                try writer.writeAll(chunk[0..count]);
                self.toss(count);
                remaining -= count;
                continue;
            }

            try self.cancel.throwIfRequested();
            if (self.vtable.stream) |f| {
                const count = try f(self, writer, .{ .bytes = remaining });
                if (count == 0) return error.ProgressRequired;
                remaining -= count;
                continue;
            }

            try self.fillMore();
        }
    }
};

pub const Writer = struct {
    sink: *anyopaque,
    vtable: *const VTable,

    // Borrowed storage.  The writer does not own this memory and does not
    // choose its size.
    buffer: []u8,

    // Simple model: pending bytes are buffer[0..end].
    end: usize = 0,

    // Open question: should writer also have seek?
    //
    // A seek field would make pending bytes buffer[seek..end], mirroring
    // Reader.  That could avoid memmove after a partial drain and make it
    // possible for drain implementations to consume a prefix incrementally.
    // It also complicates the hot path and every writable-slice invariant.
    // Start with only end unless partial drains become central enough to
    // justify putting that state north of the vtable.
    //
    // seek: usize = 0,

    cancel: *Cancellation,

    pub const VTable = struct {
        // Drain the writer's buffered bytes followed by data.
        //
        // The implementation owns updates to w.end: it can consume all,
        // consume a prefix, or leave buffered bytes in place.  The returned
        // count is how many bytes were consumed from data, not from w.buffer.
        // Flush is the semantic operation that insists all pending bytes reach
        // the sink.
        drain: *const fn (w: *Writer, data: []const []const u8) anyerror!usize,

        // Preserve the final preserve bytes while making capacity bytes of
        // unused space available.  This is useful for encoders/checksummers
        // that need a suffix from the previous write.
        rebase: ?*const fn (w: *Writer, preserve: usize, capacity: usize) anyerror!void = null,

        flush: ?*const fn (w: *Writer) anyerror!void = null,
    };

    pub fn buffered(self: *Writer) []u8 {
        return self.buffer[0..self.end];
    }

    pub fn unusedCapacity(self: *Writer) []u8 {
        return self.buffer[self.end..];
    }

    pub fn ensureUnusedCapacity(self: *Writer, n: usize) !void {
        if (self.unusedCapacity().len >= n) return;
        try self.rebase(0, n);
    }

    pub fn rebase(self: *Writer, preserve: usize, capacity: usize) !void {
        if (self.buffer.len - self.end >= capacity) return;
        if (self.vtable.rebase) |f| return f(self, preserve, capacity);

        while (self.buffer.len - self.end < capacity) {
            try self.cancel.throwIfRequested();
            const before = self.end;
            _ = try self.vtable.drain(self, &.{});
            if (before == self.end and self.buffer.len - self.end < capacity) {
                return error.ProgressRequired;
            }
        }

        if (preserve != 0) {
            // Placeholder for the "preserve suffix" variant.  The actual C++
            // implementation needs precise invariants before this becomes API.
            return error.Unimplemented;
        }
    }

    // Borrowed writable view.  Caller must call advance after writing bytes.
    pub fn writable(self: *Writer, n: usize) ![]u8 {
        try self.ensureUnusedCapacity(n);
        return self.unusedCapacity()[0..n];
    }

    pub fn advance(self: *Writer, n: usize) void {
        self.end += n;
    }

    pub fn undo(self: *Writer, n: usize) void {
        self.end -= n;
    }

    pub fn writeAll(self: *Writer, bytes: []const u8) !void {
        var rest = bytes;
        while (rest.len != 0) {
            const dst = self.unusedCapacity();
            if (dst.len >= rest.len) {
                @memcpy(dst[0..rest.len], rest);
                self.end += rest.len;
                return;
            }

            if (dst.len != 0) {
                @memcpy(dst, rest[0..dst.len]);
                self.end += dst.len;
                rest = rest[dst.len..];
            }

            try self.cancel.throwIfRequested();
            const n = try self.vtable.drain(self, &.{rest});
            rest = rest[n..];
            if (n == 0) return error.ProgressRequired;
        }
    }

    pub fn flush(self: *Writer) !void {
        if (self.vtable.flush) |f| return f(self);
        while (self.end != 0) {
            try self.cancel.throwIfRequested();
            const before = self.end;
            _ = try self.vtable.drain(self, &.{});
            if (before == self.end and self.end != 0) return error.ProgressRequired;
        }
    }
};

pub const Limit = union(enum) {
    all,
    bytes: usize,
};

pub const ReadResult = union(enum) {
    bytes: usize,
    eof,
};

fn indexOf(haystack: []const u8, needle: []const u8) ?usize {
    _ = haystack;
    _ = needle;
    return null;
}

// Desired parser style once the C++ version exists:
//
//     const raw_head = try await reader.takeUntil("\r\n\r\n");
//     const head = parseHttpResponseHead(raw_head);
//
//     if (head.isChunked()) {
//         try await readChunkedBody(&reader, on_chunk);
//     } else if (head.content_length) |n| {
//         try await reader.streamExact(&writer, n);
//     }
//
// The HTTP parser should not receive an "initial" leftover span.  The reader
// should retain read-ahead bytes internally across the response-head, body, SSE,
// and JSON parsing stages.  Range/view adapters can still be the grammar tools;
// the coroutine suspension point is simply how a failed fill gets more bytes
// without turning every parser into an explicit state-machine dispatcher.
