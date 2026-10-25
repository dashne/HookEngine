# handleJHMethod — decomp-integration spec

**Status:** BLOCKED on IDA decomp of `sub_29E168` (libkxqpplatform.so).
**Purpose:** make `handleJHMethod` a 30-minute edit the moment the decomp lands,
instead of a multi-hour discovery session.
**Enforced by:** Rule 7 (IDA-first when behavior is uncertain).
**Do not modify `Java_com_ims134_hookengine_ProxyBridge_handleJHMethod`'s C++
body — including any of the scaffolding helpers it would use — until the
required decomp evidence below is in hand.**

---

## 1. What we have

JNI surface, from `ProxyBridge.cpp`:

```cpp
extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ProxyBridge_handleJHMethod(
    JNIEnv* env, jobject,
    jobjectArray methodArr,   // reflect.Method[]
    jobject      obj,         // receiver
    jobject      obj2,        // ??? — purpose unknown
    jobjectArray objArr);     // boxed arguments, one per parameter

Return: jobject — expected to be the boxed result of the dispatched method,
or nullptr for void methods, or nullptr/thrown-exception for null returns.

Current C++ body: return nullptr; — stub.

Scaffolding helpers exist in ProxyBridge.cpp (ReflectIds, BoxResult,
IsStaticMethod, BuildDescriptor, UnwrapArgs, Rids, TagForClassName).
They are compiled but unreached by handleJHMethod. They have been audited
in Session 13 (leaks fixed) but not decomp-matched.

Sibling method nativeCNVM (sub_30BF7C) is decomp-verified and shares helper
infrastructure with what handleJHMethod will need. That gives us a partial
map — but sub_30BF7C is nativeCNVM, not handleJHMethod. Prior evidence
does not transfer without confirmation.

2. What sub_29E168 (the reference handleJHMethod) must answer
Each question has a decision tree: the decomp must show one of a small
number of concrete patterns, and each pattern maps to a specific C++ change.

Q1 — Dispatch path
What we assume but have not verified: the reference calls the target method
via Call<X>MethodV or CallNonvirtual<X>MethodV, unwrapping each boxed arg
via Call<Primitive>Value on the box's valueOf counterpart, then boxing the
return value for primitives.

Decomp evidence needed:

Locate the call to art::ArtMethod::Invoke or an equivalent JNI-side
dispatcher. Identify the jmethodID source: FromReflectedMethod(methodArr[0])?
Or a cached lookup keyed off methodArr[0]'s name/signature?

Identify the exact JNI call: CallObjectMethodA, CallStaticObjectMethodA,
CallNonvirtualObjectMethodA, or a lower-level Invoke wrapper.

Decision tree:

Decomp shows	C++ action
CallNonvirtualObjectMethodA(obj, cls, mid, args)	Current nativeCNVM pattern — port as-is
CallObjectMethodA(obj, mid, args) (virtual)	Use virtual dispatch; drop cls from the call
CallStaticObjectMethodA(cls, mid, args) for static	Add static-vs-instance branch keyed on IsStaticMethod
Direct ArtMethod::Invoke(...)	Significant rewrite — document and escalate
Q2 — obj2 semantic
Current C++: argument is named /*obj2*/ and not read.

Decomp evidence needed: identify what param_4 (the fourth argument to
sub_29E168, corresponding to our obj2) is used for. Is it:

Unused — current C++ is correct, no change.

The declaring class of methodArr[0] — used for static dispatch or for
GetObjectClass-avoidance.

A fallback receiver — dispatched to when obj is null (static path).

ActivityThread / context — for a reflection cache lookup.

Decision tree:

Decomp shows	C++ action
No reads of param_4	Keep /*obj2*/ — no change
Read at call site as class → Call<X>MethodA(cls, ...)	Use obj2 as cls argument
Read as alternative receiver	Add if (obj) dispatch(obj) else dispatch(obj2)
Read as cache key / config	Port the lookup; document purpose in header comment
Q3 — Return-value boxing
Current C++ (dead code): BoxResult uses cached valueOf jmethodIDs and
cached class refs (r.clsInteger, etc.) to box primitives. Voids return
nullptr. Objects return raw.

Decomp evidence needed: what does sub_29E168 return for the following
cases?

target is void → return value?

target returns int → Integer.valueOf(int), or nullptr, or raw jint-as-jobject?

target returns String → raw jobject, or NewGlobalRef(...), or raw local?

target returns null → nullptr, or java.lang.Void?

Decision tree:

Decomp shows	C++ action
Integer.valueOf(...) / Long.valueOf(...) / etc. per primitive	Current BoxResult matches — port as-is
Box class refs cached in a global struct (not FindClass per call)	Also matches — Rids already does this
FindClass(...) + GetStaticMethodID(..., "valueOf", ...) per call	Reproduce the leak faithfully (Rule 5)
Returns raw primitive cast to jobject	Highly unusual; document and escalate
void returns nullptr	Current behavior
void throws / returns sentinel object	Match
Q4 — Exception semantics
Current C++: relies on JNI convention — target exceptions stay pending,
caller checks.

Decomp evidence needed: after the dispatch call, does sub_29E168:

Call env->ExceptionCheck() and clear/rethrow?

Return a fallback (e.g. nullptr) on exception without clearing?

Do nothing (JNI default — exception pending, return value undefined)?

Decision tree:

Decomp shows	C++ action
No ExceptionCheck after dispatch	Match — do nothing, exception pending (Rule 5)
ExceptionClear() + fallback	Match exactly — same fallback value
ExceptionDescribe + ExceptionClear	Match — add describe call
Custom exception wrapping	Larger change — document and escalate
Q5 — Cache(env) invocation
Current C++ (dead code): UnwrapArgs calls Cache(env) at entry to resolve
valueOf jmethodIDs.

Decomp evidence needed: does sub_29E168 call the equivalent of
sub_30C238 (the unwrap cache initializer)? And when:

At function entry — always.

Only if a primitive tag is present — conditionally.

Never — relies on nativeCNVM having initialized it in the same process.

Decision tree:

Decomp shows	C++ action
Entry-level sub_30C238(...) call	Match — keep Cache(env) in handleJHMethod entry
Conditional (per-tag) call	Add per-tag guard around Cache
No call	Remove Cache(env) — trust nativeCNVM initialized it (risky if nativeCNVM never called; document)
Q6 — Descriptor derivation
Current C++ (dead code): BuildDescriptor reflectively derives a
descriptor by iterating getParameterTypes() and getReturnType() and mapping
each to a single-char tag (B, C, D, F, I, J, L, S, Z).

Decomp evidence needed: does sub_29E168 derive a descriptor at all? If
so, from what source:

Reflective Class.getParameterTypes() iteration.

Method name + ART-side ArtMethod::GetSignature() (native method).

A pre-built descriptor passed by caller in obj2 or methodArr[1].

Decision tree:

Decomp shows	C++ action
Reflective parameter-type iteration, class.getName() per param	Current BuildDescriptor matches
ArtMethod::GetSignature() (JNI-side ART access)	Larger change — document, likely requires art::ArtMethod field offsets
Descriptor passed in by caller	Use the caller-provided descriptor; drop BuildDescriptor
Q7 — methodArr arity / element use
Current C++: methodArr is treated as reflect.Method[] with exactly one
element (target method).

Decomp evidence needed: does sub_29E168 iterate methodArr? If so, for
what:

Only [0] used (target method).

[0] is target, [1..] are super-methods for virtual dispatch chain.

[0] is target, [1] is declaring class, [2..] are param types.

Decision tree:

Decomp shows	C++ action
Only index 0 accessed	Current assumption correct
Multiple elements with specific semantic	Port the multi-index logic — document each slot
3. What decomp evidence is required to unblock
The decomp artifact delivered must clearly show, for sub_29E168:

Signature: exact parameter count and types ((JNIEnv*, jclass, jobjectArray, jobject, jobject, jobjectArray, ...) — confirm arity matches our 6 args or if there's a 7th we missed).

Entry cache call — whether sub_30C238 is called and under what condition.

methodArr indexing — GetObjectArrayElement calls and their index arguments.

obj2 reads — first dereference of the fourth param.

Descriptor derivation — how tags are generated and in what order.

Per-arg unwrapping loop — tag switch structure and which Call<X>Value methods each tag maps to.

Static-vs-instance branch — the branch condition (IsStaticMethod? direct flag test? obj == nullptr check?).

Target dispatch call — the exact JNI call variant and its argument order.

Return boxing — the valueOf calls (or lack thereof) on the return path.

Exception handling — any ExceptionCheck/ExceptionClear after dispatch.

If any of these are absent from the decomp artifact, Session 16 cannot land
handleJHMethod with a clean conscience (Rule 5). Instead, document the
partial evidence and request a re-dump.

4. Existing helper functions — audit pending decomp
The helpers in ProxyBridge.cpp were audited for local-ref leaks in Session 13
and are believed correct. But their exact behavior (e.g., whether
BuildDescriptor matches reference tag derivation) is unverified until Q6 is
answered. If Q6 shows a divergence, the helper changes even though it is not
currently called.

Therefore: do not modify any helper's behavior before Q6 resolves. Do not
add tests that pin a helper's current behavior — they'd pin wrong behavior.

5. Session-open criteria for Session 16
Session 16 can begin handleJHMethod work if and only if the decomp
artifact contains the ten evidence points in §3. Otherwise:

Session 16 shifts to the next Phase A item that is decomp-ready, or

Session 16 is a diagnosis-only session.

Do not attempt to fill in missing evidence by inference. Rule 7 exists for a
reason.

**Verify before commit:**

```powershell
# Should be >5000 bytes now
Get-ChildItem .\HANDLEJHMETHOD_DECOMP_SPEC.md | Select-Object Length

# Should return ≥ 7 matches (Q1 through Q7)
Select-String -Path .\HANDLEJHMETHOD_DECOMP_SPEC.md -Pattern '^### Q[1-7]'

# Should return 1 match (the exit-criteria section)
Select-String -Path .\HANDLEJHMETHOD_DECOMP_SPEC.md -Pattern '## 3\. What decomp evidence is required'