#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""
parse_capture.py — extract the Phase-2 facts from a renderdoc capture that
was converted to XML (`renderdoccmd convert -f cap.rdc -o out.xml -c xml`).

Reconstructs, from the serialized Vulkan stream:
  * buffers (size/usage) and their memory bindings (memory id + offset)
  * descriptor writes of UNIFORM_BUFFER[_DYNAMIC] type -> (set, binding,
    buffer, offset, range)
  * the frame's command stream skeleton: render passes (with framebuffer
    dimensions/attachments), pipeline binds, descriptor binds (with dynamic
    offsets), draws/dispatches, and queue submits
  * coherent mapped-memory writes (the uniform-streaming mechanism)

Output: a human-readable report on stdout. Use --json for machine output.
"""

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict


# ------------------------- generic value readers ---------------------------

def child(el, name):
    for c in el:
        if c.get("name") == name:
            return c
    return None


def val(el, name, default=None):
    c = child(el, name)
    if c is None:
        return default
    if c.tag in ("uint", "int", "float"):
        try:
            return int(c.text)
        except (TypeError, ValueError):
            try:
                return float(c.text)
            except (TypeError, ValueError):
                return default
    if c.tag == "ResourceId":
        return int(c.text)
    if c.tag == "enum":
        return c.get("string", c.text)
    if c.tag == "string":
        return c.text
    return c


def enum_num(el, name, default=0):
    c = child(el, name)
    if c is None or c.text is None:
        return default
    try:
        return int(c.text)
    except ValueError:
        return default


# ------------------------------- the parse --------------------------------

class Capture:
    def __init__(self):
        self.buffers = {}          # id -> {size, usage}
        self.buf_mem = {}          # buffer id -> (memory id, offset)
        self.images = {}           # id -> {w,h,format,usage,samples}
        self.views = {}            # view id -> image id
        self.renderpasses = {}     # id -> {attachments:[formats]}
        self.framebuffers = {}     # id -> {rp, w, h, attachments:[view ids]}
        self.ubo_writes = []       # descriptor writes of UBO type
        self.stream = []           # frame command stream (flat, per cmdbuf)
        self.submits = []          # queue submits: [cmdbuf ids]
        self.mapped_writes = []    # coherent mapped writes {memory,offset,size}
        self.swapchain_images = [] # image ids
        self.desc_sets = {}        # set id -> [slots] (from Initial Contents)

    # -- creation-time chunks --
    def on_create_buffer(self, ch):
        ci = child(ch, "CreateInfo")
        bid = val(ch, "Buffer")
        self.buffers[bid] = {
            "size": val(ci, "size"),
            "usage": val(ci, "usage"),
        }

    def on_bind_buffer_memory(self, ch):
        bid = val(ch, "buffer")
        self.buf_mem[bid] = (val(ch, "memory"), val(ch, "memoryOffset", 0))

    def on_create_image(self, ch):
        ci = child(ch, "CreateInfo")
        ext = child(ci, "extent")
        iid = val(ch, "Image")
        self.images[iid] = {
            "w": val(ext, "width"), "h": val(ext, "height"),
            "format": val(ci, "format"), "usage": val(ci, "usage"),
            "samples": val(ci, "samples"),
        }

    def on_create_image_view(self, ch):
        ci = child(ch, "CreateInfo")
        self.views[val(ch, "View")] = val(ci, "image")

    def on_create_renderpass(self, ch):
        ci = child(ch, "CreateInfo")
        atts = []
        arr = child(ci, "pAttachments")
        if arr is not None:
            for st in arr:
                atts.append(val(st, "format"))
        self.renderpasses[val(ch, "RenderPass")] = {"attachments": atts}

    def on_create_framebuffer(self, ch):
        ci = child(ch, "CreateInfo")
        views = []
        arr = child(ci, "pAttachments")
        if arr is not None:
            views = [int(r.text) for r in arr if r.tag == "ResourceId"]
        self.framebuffers[val(ch, "Framebuffer")] = {
            "rp": val(ci, "renderPass"),
            "w": val(ci, "width"), "h": val(ci, "height"),
            "attachments": views,
        }

    def on_swapchain_images(self, ch):
        arr = child(ch, "pSwapchainImages")
        img = val(ch, "SwapchainImage")
        if img is not None:
            self.swapchain_images.append(img)
        if arr is not None:
            for r in arr:
                if r.tag == "ResourceId":
                    self.swapchain_images.append(int(r.text))

    def on_update_descriptor_sets(self, ch):
        arr = child(ch, "pDescriptorWrites")
        if arr is None:
            return
        for w in arr:
            dtype = val(w, "descriptorType") or ""
            if "UNIFORM_BUFFER" not in str(dtype) and "STORAGE_BUFFER" not in str(dtype):
                continue
            binfo = child(w, "pBufferInfo")
            entries = []
            if binfo is not None:
                for st in binfo:
                    entries.append({
                        "buffer": val(st, "buffer"),
                        "offset": val(st, "offset"),
                        "range": val(st, "range"),
                    })
            self.ubo_writes.append({
                "set": val(w, "dstSet"),
                "binding": val(w, "dstBinding"),
                "type": str(dtype),
                "buffers": entries,
            })

    # -- frame-stream chunks --
    def on_stream(self, name, ch):
        e = {"op": name, "cb": val(ch, "commandBuffer")}
        if name == "vkCmdBeginRenderPass":
            bi = child(ch, "RenderPassBegin")
            if bi is not None:
                e["rp"] = val(bi, "renderPass")
                e["fb"] = val(bi, "framebuffer")
        elif name == "vkCmdBindPipeline":
            e["pipeline"] = val(ch, "pipeline")
            e["bindpoint"] = str(val(ch, "pipelineBindPoint"))
        elif name == "vkCmdBindDescriptorSets":
            e["firstSet"] = val(ch, "firstSet")
            sets = child(ch, "pDescriptorSets")
            e["sets"] = [int(r.text) for r in sets if r.tag == "ResourceId"] if sets is not None else []
            dyn = child(ch, "pDynamicOffsets")
            e["dyn"] = [int(u.text) for u in dyn if u.tag == "uint"] if dyn is not None else []
        elif name in ("vkCmdDraw", "vkCmdDrawIndexed"):
            e["verts"] = val(ch, "vertexCount") or val(ch, "indexCount")
            e["inst"] = val(ch, "instanceCount")
        elif name == "vkCmdDispatch":
            e["groups"] = (val(ch, "groupCountX"), val(ch, "groupCountY"), val(ch, "groupCountZ"))
        elif name == "vkCmdSetViewport":
            vp = child(ch, "pViewports")
            if vp is not None:
                first = next(iter(vp), None)
                if first is not None:
                    e["viewport"] = (val(first, "x"), val(first, "y"),
                                     val(first, "width"), val(first, "height"))
        elif name == "vkBeginCommandBuffer":
            e["cmdbuf"] = val(ch, "commandBuffer")
        self.stream.append(e)

    def on_queue_submit(self, ch):
        subs = child(ch, "pSubmits")
        bufs = []
        if subs is not None:
            for s in subs:
                arr = child(s, "pCommandBuffers")
                if arr is not None:
                    bufs += [int(r.text) for r in arr if r.tag == "ResourceId"]
        self.submits.append(bufs)

    def on_mapped_write(self, ch):
        mr = child(ch, "MemRange")
        self.mapped_writes.append({
            "memory": val(mr, "memory"),
            "offset": val(mr, "offset"),
            "size": val(mr, "size"),
        })


STREAM_OPS = {
    "vkBeginCommandBuffer", "vkCmdBeginRenderPass", "vkCmdEndRenderPass",
    "vkCmdBindPipeline", "vkCmdBindDescriptorSets", "vkCmdDraw",
    "vkCmdDrawIndexed", "vkCmdDispatch", "vkCmdSetViewport",
    "vkCmdCopyImage", "vkCmdCopyBuffer", "vkCmdPipelineBarrier",
    "vkEndCommandBuffer",
}

def _on_initial_contents(cap, ch):
    # Descriptor-set state is serialized as Initial Contents, not API calls.
    t = val(ch, "type")
    if str(t) != "eResDescriptorSet":
        return
    sid = val(ch, "id")
    slots = []
    arr = child(ch, "Bindings")
    if arr is not None:
        for st in arr:
            slots.append({
                "type": str(val(st, "type")),
                "buffer": val(st, "resource"),
                "offset": val(st, "offset"),
                "range": val(st, "range"),
            })
    cap.desc_sets[sid] = slots


HANDLERS = {
    "Internal::Initial Contents": _on_initial_contents,
    "vkCreateBuffer": Capture.on_create_buffer,
    "vkBindBufferMemory": Capture.on_bind_buffer_memory,
    "vkCreateImage": Capture.on_create_image,
    "vkCreateImageView": Capture.on_create_image_view,
    "vkCreateRenderPass": Capture.on_create_renderpass,
    "vkCreateFramebuffer": Capture.on_create_framebuffer,
    "vkGetSwapchainImagesKHR": Capture.on_swapchain_images,
    "vkUpdateDescriptorSets": Capture.on_update_descriptor_sets,
    "vkQueueSubmit": Capture.on_queue_submit,
    "Internal::Coherent Mapped Memory Write": Capture.on_mapped_write,
}


def parse(path):
    cap = Capture()
    for _, el in ET.iterparse(path, events=("end",)):
        if el.tag != "chunk":
            continue
        name = el.get("name")
        h = HANDLERS.get(name)
        if h:
            h(cap, el)
        elif name in STREAM_OPS:
            cap.on_stream(name, el)
        el.clear()
    return cap


# ------------------------------- reporting --------------------------------

def report(cap):
    p = print
    p("== buffers with UNIFORM usage ==")
    for bid, b in sorted(cap.buffers.items()):
        if b["usage"] and "UNIFORM" in str(b["usage"]):
            mem = cap.buf_mem.get(bid, ("?", "?"))
            p(f"  buffer {bid}: size {b['size']:>12,}  mem {mem[0]}+{mem[1]}  usage {b['usage']}")

    p("\n== descriptor writes of UBO/SSBO type (set, binding -> buffer+off/range) ==")
    seen = defaultdict(int)
    for w in cap.ubo_writes:
        for e in w["buffers"]:
            key = (w["binding"], w["type"], e["buffer"], e["range"])
            seen[key] += 1
    for (binding, dtype, buf, rng), n in sorted(seen.items()):
        p(f"  binding {binding:>2}  {dtype:<42} buffer {buf}  range {rng}  (x{n})")

    p("\n== render passes in frame order (with framebuffer size) ==")
    i = 0
    draws = disp = 0
    cur = None
    for e in cap.stream:
        if e["op"] == "vkCmdBeginRenderPass":
            if cur:
                p(f"     .. draws {cur['draws']}, dispatch {cur['disp']}")
            fb = cap.framebuffers.get(e.get("fb"), {})
            rp = cap.renderpasses.get(e.get("rp"), {})
            i += 1
            cur = {"draws": 0, "disp": 0}
            p(f"  pass {i:>2}: fb {e.get('fb')} {fb.get('w')}x{fb.get('h')} "
              f"atts {len(fb.get('attachments', []))} formats {rp.get('attachments')}")
        elif e["op"] in ("vkCmdDraw", "vkCmdDrawIndexed"):
            draws += 1
            if cur:
                cur["draws"] += 1
        elif e["op"] == "vkCmdDispatch":
            disp += 1
            if cur:
                cur["disp"] += 1
    if cur:
        p(f"     .. draws {cur['draws']}, dispatch {cur['disp']}")
    p(f"  TOTAL: {draws} draws, {disp} dispatches, {i} passes")

    p("\n== camera-UBO hunt: UBO slots weighted by following draw count ==")
    # Walk the stream: track currently-bound sets; when a draw occurs, credit
    # every UBO slot of every bound set. The slot credited by ~all scene draws
    # at a stable (buffer, offset) is the per-view camera-constants block.
    bound = {}
    credit = defaultdict(int)   # (buffer, offset, range) -> draws
    setcount = defaultdict(set) # (buffer, offset, range) -> distinct sets
    for e in cap.stream:
        if e["op"] == "vkCmdBindDescriptorSets":
            first = e.get("firstSet") or 0
            for i, s in enumerate(e.get("sets", [])):
                bound[first + i] = s
        elif e["op"] in ("vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDispatch"):
            for slot_idx, s in bound.items():
                for b in cap.desc_sets.get(s, []):
                    if "UNIFORM_BUFFER" not in b["type"]:
                        continue
                    key = (b["buffer"], b["offset"], b["range"])
                    credit[key] += 1
                    setcount[key].add(s)
    top = sorted(credit.items(), key=lambda kv: -kv[1])[:15]
    p("  (buffer, offset, range) -> draws credited, distinct sets")
    for (buf, off, rng), n in top:
        mem = cap.buf_mem.get(buf, ("?", 0))
        p(f"  buffer {buf:>6} off {off:>8} range {rng:>6}  draws {n:>4}  sets {len(setcount[(buf,off,rng)]):>4}"
          f"   [mem {mem[0]}+{mem[1]}]")

    p("\n== descriptor-set UBO slot inventory (by range) ==")
    rng_hist = defaultdict(int)
    for slots in cap.desc_sets.values():
        for b in slots:
            if "UNIFORM_BUFFER" in b["type"]:
                rng_hist[b["range"]] += 1
    for rng, n in sorted(rng_hist.items(), key=lambda kv: -kv[1])[:15]:
        p(f"  range {rng:>7}: x{n}")

    p("\n== dynamic-offset usage (per bind count histogram) ==")
    hist = defaultdict(int)
    for e in cap.stream:
        if e["op"] == "vkCmdBindDescriptorSets":
            hist[len(e.get("dyn", []))] += 1
    for n, c in sorted(hist.items()):
        p(f"  {n} dynamic offsets: x{c}")

    p("\n== coherent mapped-memory writes ==")
    for m in cap.mapped_writes:
        p(f"  memory {m['memory']}  offset {m['offset']:,}  size {m['size']:,}")

    p("\n== queue submits (command buffers) ==")
    for i, s in enumerate(cap.submits):
        p(f"  submit {i}: cmdbufs {s}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xml", help="XML converted from .rdc")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    cap = parse(args.xml)
    if args.json:
        json.dump({
            "buffers": cap.buffers, "buf_mem": cap.buf_mem,
            "ubo_writes": cap.ubo_writes, "framebuffers": cap.framebuffers,
            "submits": cap.submits, "mapped": cap.mapped_writes,
        }, sys.stdout, indent=1, default=str)
    else:
        report(cap)


if __name__ == "__main__":
    main()
