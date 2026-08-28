from pathlib import Path
import re, sys

ROOT=Path('.')
BUS_C=ROOT/'src/tab5_guest_bus.c'; BUS_H=ROOT/'src/tab5_guest_bus.h'
C=ROOT/'src/tab5_compose.c'; H=ROOT/'src/tab5_compose.h'
W=ROOT/'components/px68k/libretro/windraw.c'; M=ROOT/'src/main.c'
PKG=Path(__file__).resolve().parent
TAG='PX68K_R57D_ORDERED_SHADOW_HOLD'
MARK='PX68K_R57D: class-certified visible TEXT + ordered CPU0 BG/Sprite shadow ownership ACTIVE; CPU1 never waits'


def fail(msg):
    print('R57d BIG DETACH patch ERROR:',msg); raise SystemExit(2)

def span(text, sig):
    p=text.find(sig)
    if p<0: fail('function not found: '+sig)
    b=text.find('{',p)
    if b<0: fail('opening brace not found: '+sig)
    dep=0; i=b; ls=lc=ll=lb=False; esc=False
    while i<len(text):
        ch=text[i]; nx=text[i+1] if i+1<len(text) else ''
        if ll:
            if ch=='\n': ll=False
        elif lb:
            if ch=='*' and nx=='/': lb=False; i+=1
        elif ls:
            if esc: esc=False
            elif ch=='\\': esc=True
            elif ch=='"': ls=False
        elif lc:
            if esc: esc=False
            elif ch=='\\': esc=True
            elif ch=="'": lc=False
        else:
            if ch=='/' and nx=='/': ll=True; i+=1
            elif ch=='/' and nx=='*': lb=True; i+=1
            elif ch=='"': ls=True
            elif ch=="'": lc=True
            elif ch=='{': dep+=1
            elif ch=='}':
                dep-=1
                if dep==0: return p,i+1
        i+=1
    fail('unclosed function: '+sig)

def decl_span(text,name):
    m=re.search(r'(^[^\n]*\b'+re.escape(name)+r'\s*\()',text,re.M)
    if not m: fail('declaration/definition not found: '+name)
    start=m.start(); p=text.find('(',m.start()); dep=0
    for i in range(p,len(text)):
        if text[i]=='(': dep+=1
        elif text[i]==')':
            dep-=1
            if dep==0: return start,i+1
    fail('signature close not found: '+name)

def def_sig_span(text,name):
    # Find a real definition, never a preceding prototype. Match each name(
    # candidate, brace-match its parameter list, then require `{` after only
    # whitespace/newlines.
    for m in re.finditer(r'\b'+re.escape(name)+r'\s*\(',text):
        line=text.rfind('\n',0,m.start())+1
        p=text.find('(',m.start())
        dep=0; close=-1
        for i in range(p,len(text)):
            if text[i]=='(': dep+=1
            elif text[i]==')':
                dep-=1
                if dep==0: close=i+1; break
        if close<0: continue
        j=close
        while j<len(text) and text[j].isspace(): j+=1
        if j<len(text) and text[j]=='{': return line,close
    fail('function definition not found: '+name)

def one(s,old,new,label):
    n=s.count(old)
    if n!=1: fail(f'{label}: expected 1 anchor, found {n}')
    return s.replace(old,new,1)

def patch_func(text,sig,fn):
    a,b=span(text,sig); old=text[a:b]; new=fn(old)
    if new==old: fail('no change in '+sig)
    return text[:a]+new+text[b:]

for p in (BUS_C,BUS_H,C,H,W,M):
    if not p.is_file(): fail('missing '+str(p))

bc=BUS_C.read_text(encoding='utf-8'); bh=BUS_H.read_text(encoding='utf-8')
cs=C.read_text(encoding='utf-8'); hs=H.read_text(encoding='utf-8')
ws=W.read_text(encoding='utf-8'); ms=M.read_text(encoding='utf-8')

if MARK in ms and TAG in bc and 'PX68K_R57D_CLASS_VALIDATE' in cs:
    print('R57d BIG DETACH already applied and verified'); raise SystemExit(0)
if 'PX68K_R57C: ordered raster-token boundaries ACTIVE' not in ms: fail('R57c main lineage missing')
if 'PX68K_R57C_RASTER_TOKEN' not in bc: fail('R57c bus lineage missing')
if 'PX68K_R56S5K_VISIBLE_TEXT_QUARANTINE' not in ws: fail('R56s5k visible-text correctness fence missing')
if 'r56s5_render_host_bt_exact' not in cs: fail('R56s5 host-BT helper missing')
if 'tab5_guest_bus_post_raster(VLINE);' not in ws: fail('R57c raster token post missing')

# Replace our bus files wholesale. They are package-owned since R57a.
bc=(PKG/'r57d_bus.c').read_text(encoding='utf-8')
bh=(PKG/'r57d_bus.h').read_text(encoding='utf-8')

# ---- compose global state / class state ----
anchor='static volatile uint32_t s_r56s5_hostbt_state;\n'
if cs.count(anchor)!=1: fail('s_r56s5_hostbt_state anchor mismatch')
cs=cs.replace(anchor,anchor+r'''/* PX68K_R57D_CLASS_VALIDATE: state 0=pending, 2=certified, 3=failed. */
#define R57D_CLASS_COUNT 1024u
#define R57D_CLASS_PASS_NEED 8u
static uint8_t *s_r57d_class_state;
static uint8_t *s_r57d_class_pass;
static uint32_t s_r57d_slot_hold[TAB5_COMPOSE_MAX_SLOTS];
static uint32_t s_r57d_burst_hold[TAB5_GBT65K_BURST_SLOTS];
static volatile uint32_t s_r57d_shadow_render_active;
static const uint8_t *s_r57d_bg_render_src, *s_r57d_c8_render_src;
static const uint8_t *s_r57d_c16_render_src, *s_r57d_sprite_render_src;
static volatile uint32_t s_r57d_hold_wait_fail;
''',1)

# Class memory init after bus init (R57a inserted this call).
init_anchor='    (void)tab5_guest_bus_init(); /* observation-only; allocation failure keeps R56s5k exact path */'
if init_anchor not in cs:
    # tolerate comment changes
    init_anchor='    (void)tab5_guest_bus_init();'
if cs.count(init_anchor)!=1: fail('guest bus init anchor mismatch')
cs=cs.replace(init_anchor,init_anchor+r'''
    if (!s_r57d_class_state) {
        uint8_t *p=(uint8_t*)heap_caps_aligned_calloc(64, R57D_CLASS_COUNT, 2u,
                                                      MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (p) { s_r57d_class_state=p; s_r57d_class_pass=p+R57D_CLASS_COUNT; }
        else ESP_LOGW(TAG,"PX68K_R57D: class table allocation failed; visible TEXT remains R56s5k exact");
    }
''',1)

# Getter after old getter.
a,b=span(cs,'int tab5_compose_gbt65k_hostbt_state(void)')
getter=r'''

int tab5_compose_r57d_class_state(uint16_t key)
{
    if (!tab5_guest_bus_bg_shadow_ready() || !s_r57d_class_state || key>=R57D_CLASS_COUNT) return 3;
    return (int)__atomic_load_n(&s_r57d_class_state[key],__ATOMIC_ACQUIRE);
}
'''
cs=cs[:b]+getter+cs[b:]

# CPU0 source selectors. Only active while compositor owns an ordered freeze.
sel_anchor='static void host_bg_plane8('
pos=cs.find(sel_anchor)
if pos<0: fail('host_bg_plane8 missing')
selectors=r'''static inline const uint8_t *r57d_bg_src(void)
{
    return (load_acquire(&s_r57d_shadow_render_active) && s_r57d_bg_render_src) ? s_r57d_bg_render_src : BG;
}
static inline const uint8_t *r57d_c8_src(void)
{
    return (load_acquire(&s_r57d_shadow_render_active) && s_r57d_c8_render_src) ? s_r57d_c8_render_src : BGCHR8;
}
static inline const uint8_t *r57d_c16_src(void)
{
    return (load_acquire(&s_r57d_shadow_render_active) && s_r57d_c16_render_src) ? s_r57d_c16_render_src : BGCHR16;
}
static inline const uint8_t *r57d_sprite_src(void)
{
    return (load_acquire(&s_r57d_shadow_render_active) && s_r57d_sprite_render_src) ? s_r57d_sprite_render_src : Sprite_Regs;
}

'''
cs=cs[:pos]+selectors+cs[pos:]

# Change only CPU0 host renderer helper bodies, never stock PX68K CPU1 renderer.
def shadowize(fn):
    out=fn
    out=out.replace('&BG[','&r57d_bg_src()[')
    out=out.replace('&BGCHR8[','&r57d_c8_src()[')
    out=out.replace('&BGCHR16[','&r57d_c16_src()[')
    out=out.replace('(const host_sprite_ctrl_t *)Sprite_Regs','(const host_sprite_ctrl_t *)r57d_sprite_src()')
    return out
for sig in ('static void host_bg_plane8(', 'static void host_bg_plane16(', 'static void host_sprite_priority('):
    cs=patch_func(cs,sig,shadowize)

# Insert class validator before legacy R56s5 one-line validator branch.
render_sig='static int render_gbt65k_line(compose_slot_t *slot)'
a,b=span(cs,render_sig); fn=cs[a:b]
old='''    /* R56s5 bit4 means the packet carries expanded TEXT + BG host state and\n'''
if old not in fn: fail('R56s5 render branch comment anchor missing')
validator=r'''    /* PX68K_R57D_CLASS_VALIDATE
     * Every class is first displayed from the authoritative stock CPU1 BT
     * snapshot. CPU0 builds the candidate from the ordered frozen BG/Sprite
     * shadow. Only after 8 consecutive exact lines does this class bypass the
     * CPU1 stock Text_DrawLine/BG_DrawLine path. */
    if (slot->selfcheck & 32u) {
        const uint16_t key=(uint16_t)slot->bottom_page | ((uint16_t)(slot->top_page & 3u)<<8);
        const int text_on5=(slot->selfcheck & 1u)!=0u;
        const int bg_on5=(slot->selfcheck & 2u)!=0u;
        const uint8_t *hf=NULL;
        const uint16_t *hb=r56s5_render_host_bt_exact(
            bg_on5 ? &slot->u.gbt65k.bg_state : NULL,
            slot->u.gbt65k.text_idx, slot->u.gbt65k.text_pal,
            slot->width, text_on5, &hf);
        uint32_t bad=slot->width;
        if (!hb || !hf) bad=0u;
        else for (uint32_t i=0;i<slot->width;++i) {
            if (hb[i]!=slot->u.gbt65k.exact_bg_text[i] ||
                (hf[i]&3u)!=(slot->u.gbt65k.exact_flags[i]&3u)) { bad=i; break; }
        }
        if (key<R57D_CLASS_COUNT && s_r57d_class_state) {
            if (bad==slot->width) {
                uint8_t n=s_r57d_class_pass[key];
                if (n<255u) ++n;
                s_r57d_class_pass[key]=n;
                if (n>=R57D_CLASS_PASS_NEED && s_r57d_class_state[key]!=2u) {
                    __atomic_store_n(&s_r57d_class_state[key],2u,__ATOMIC_RELEASE);
                    printf("PX68K_R57D_CLASS: CERTIFIED key=%u after=%u exact lines; visible TEXT class now CPU0 shadow-offloaded\\n",
                           (unsigned)key,(unsigned)n);
                }
            } else {
                __atomic_store_n(&s_r57d_class_state[key],3u,__ATOMIC_RELEASE); s_r57d_class_pass[key]=0u;
                printf("PX68K_R57D_CLASS: FAIL key=%u x=%lu host=%04X/%u stock=%04X/%u; class stays CPU1 exact\\n",
                       (unsigned)key,(unsigned long)bad,
                       hb?hb[bad]:0u,hf?(unsigned)(hf[bad]&3u):0u,
                       slot->u.gbt65k.exact_bg_text[bad],
                       (unsigned)(slot->u.gbt65k.exact_flags[bad]&3u));
            }
        }
        r56s4_render_gbt65k_exact_bt(slot->dst,grp_row,
                                     slot->u.gbt65k.gvram_x&511u,
                                     slot->u.gbt65k.exact_bg_text,
                                     slot->u.gbt65k.exact_flags,slot->width,
                                     slot->grp_pri,slot->bg_pri,slot->text_pri);
        ++s_gbt65k_scalar_lines;
        return 1;
    }

'''
fn=fn.replace(old,validator+old,1); cs=cs[:a]+fn+cs[b:]

# ---- signatures ----
def add_sig_args(text,name,args,definition=False):
    a,b=(def_sig_span(text,name) if definition else decl_span(text,name)); sig=text[a:b]
    if 'r57d_shadow_seq' in sig: return text
    if 'uint64_t render_ticket' not in sig: fail(name+' render_ticket signature anchor missing')
    sig2=sig.replace('uint64_t render_ticket',args+'uint64_t render_ticket',1)
    return text[:a]+sig2+text[b:]

# C definitions and header declarations. Definition lookup is brace-scoped so
# an earlier prototype can never receive the implementation-only edit.
cs=add_sig_args(cs,'tab5_compose_submit_gbt65k_line','uint32_t r57d_shadow_seq, ',True)
cs=add_sig_args(cs,'tab5_compose_submit_gbt65k_exact_bt_line','uint16_t r57d_class, uint32_t r57d_shadow_seq, ',True)
hs=add_sig_args(hs,'tab5_compose_submit_gbt65k_line','uint32_t r57d_shadow_seq, ')
# R57d1: the exact-BT submit is intentionally a private compose.c API whose
# only cross-TU declaration lives in windraw.c.  R56s5 never exported it from
# tab5_compose.h, so do NOT invent a public header declaration here.
if 'tab5_compose_submit_gbt65k_exact_bt_line' in hs:
    fail('unexpected public exact-BT declaration in tab5_compose.h; refusing ambiguous ABI rewrite')
else:
    print('R57d1 preflight: exact-BT submit is private (compose.c + windraw extern); public header correctly has no declaration')

# R57d3 function-block rewrite: do not infer how a slot was acquired.
# Bind the hold id after all slot preparation and immediately before the slot
# is published to the burst/ordinary queues.  Acquisition spelling/order is
# deliberately irrelevant.
def insert_hold_before_publication(fn,label):
    if 'PX68K_R57D3_HOLD_BIND' in fn:
        return fn
    if not re.search(r'\buint8_t\s+idx\b',fn):
        fail(label+' idx declaration missing')
    if 'queue_slot(idx)' not in fn:
        fail(label+' ordinary queue publication missing')
    ready=fn.find('gbt65k_ready_push(idx)')
    if ready<0:
        fail(label+' burst queue publication missing')
    candidates=list(re.finditer(r'(?m)^(?P<indent>\s*)if\s*\(\s*burst65k\s*\)\s*\{',fn[:ready]))
    if not candidates:
        fail(label+' burst publication owner missing')
    m=candidates[-1]
    indent=m.group('indent')
    store=(indent+'/* PX68K_R57D3_HOLD_BIND: bind selected slot immediately before publication. */\n'+
           indent+'if (burst65k) s_r57d_burst_hold[idx]=r57d_shadow_seq;\n'+
           indent+'else s_r57d_slot_hold[idx]=r57d_shadow_seq;\n')
    return fn[:m.start()]+store+fn[m.start():]

# Normal submit: remember hold and remove shared-source pending for shadow-owned jobs.
a,b=span(cs,'int tab5_compose_submit_gbt65k_line('); fn=cs[a:b]
fn=insert_hold_before_publication(fn,'normal submit')
# All bgsource pending operations in this function become shared-source-only.
fn=re.sub(r'if \(bg_on\)\s*\n(\s*)__atomic_(add_fetch|sub_fetch)\(&s_bgsource_pending',
          r'if (bg_on && !r57d_shadow_seq)\n\1__atomic_\2(&s_bgsource_pending',fn)
cs=cs[:a]+fn+cs[b:]

# Exact submit: independent R57d per-class validator, always ordinary queue while validating.
a,b=span(cs,'int tab5_compose_submit_gbt65k_exact_bt_line('); fn=cs[a:b]
oldcand=re.compile(r'''    /\* PX68K_R56S5F_VALIDATOR_NORMAL_QUEUE.*?    const int r56s5_validate = r56s5_validate_candidate;''',re.S)
m=oldcand.search(fn)
if not m: fail('R56s5f validator block not found in exact submit')
newcand=r'''    /* R57d supersedes the old global one-line validator with per-class
     * certification. Every shadow-held packet uses the same FIFO burst queue;
     * this preserves freeze order across validation and production classes.
     * Validation packets are marked bit32 and the worker exempts them from
     * stale retirement until the exact stock-vs-shadow comparison completes. */
    const int r57d_validate = s_gbt65k_burst_ready && text_on && text_src &&
        r57d_class<R57D_CLASS_COUNT && s_r57d_class_state &&
        __atomic_load_n(&s_r57d_class_state[r57d_class],__ATOMIC_ACQUIRE)==0u &&
        (!bg_on || r57d_shadow_seq!=0u);
    const int r56s5_validate_candidate = 0;
    const int burst65k = s_gbt65k_burst_ready;
    const int r56s5_validate = 0;'''
fn=fn[:m.start()]+newcand+fn[m.end():]
# selfcheck assignment block
pat=re.compile(r'''    slot->selfcheck = \(uint8_t\)\(8u \| \(r56s5_validate \? 16u : 0u\) \|\n\s*\(r56s5_validate && text_on \? 1u : 0u\) \|\n\s*\(r56s5_validate && bg_on \? 2u : 0u\)\);''')
m=pat.search(fn)
if not m: fail('exact selfcheck assignment not found')
repl=r'''    slot->selfcheck = (uint8_t)(8u | (r57d_validate ? 32u : 0u) |
                                (r57d_validate && text_on ? 1u : 0u) |
                                (r57d_validate && bg_on ? 2u : 0u));
    slot->bottom_page=(uint8_t)(r57d_class&0xffu);
    slot->top_page=(uint8_t)((r57d_class>>8)&3u);'''
fn=fn[:m.start()]+repl+fn[m.end():]
# copy validator sources under R57d condition
fn=fn.replace('    if (r56s5_validate) {\n','    if (r57d_validate) {\n',1)
# Store only after exact slot acquisition has succeeded. This uses the same
# structural slot-initialization anchor as the normal submit and is independent
# of optional profiling locals (`cc0` may be absent in the current R57c tree).
fn=insert_hold_before_publication(fn,'exact submit')
cs=cs[:a]+fn+cs[b:]

# ---- compose worker ordered wait/release ----
a,b=span(cs,'static void compose_task(void *arg)'); fn=cs[a:b]
slotm=re.search(r'(?m)^(?P<indent>\s*)compose_slot_t\s*\*\s*slot\s*=.*?;\s*$',fn)
if not slotm: fail('compose worker slot-pointer structural anchor missing')
pre=r'''
        uint32_t r57d_hold_seq=0u;
        if (slot->type==COMPOSE_JOB_GBT65K) {
            r57d_hold_seq=burst65k ? s_r57d_burst_hold[idx] : s_r57d_slot_hold[idx];
            if (r57d_hold_seq) {
                if (tab5_guest_bus_shadow_hold_wait(r57d_hold_seq)) {
                    s_r57d_bg_render_src=tab5_guest_bus_bg_shadow();
                    s_r57d_c8_render_src=tab5_guest_bus_bgchr8_shadow();
                    s_r57d_c16_render_src=tab5_guest_bus_bgchr16_shadow();
                    s_r57d_sprite_render_src=tab5_guest_bus_sprite_shadow();
                    store_release(&s_r57d_shadow_render_active,1u);
                } else {
                    __atomic_add_fetch(&s_r57d_hold_wait_fail,1u,__ATOMIC_RELAXED);
                    printf("PX68K_R57D: HOLD WAIT FAIL seq=%lu; shared fallback used for this line\\n",
                           (unsigned long)r57d_hold_seq);
                }
            }
        }
'''
fn=fn[:slotm.end()]+pre+fn[slotm.end():]
# Release immediately after source pending decrement area, before slot goes free. Find first free-push logic after branch.
release_anchor='''        if (burst65k) {\n            if (!gbt65k_free_push(idx))'''
if release_anchor not in fn: fail('worker burst free anchor missing')
release=r'''        if (r57d_hold_seq) {
            store_release(&s_r57d_shadow_render_active,0u);
            tab5_guest_bus_shadow_hold_release(r57d_hold_seq);
            if (burst65k) s_r57d_burst_hold[idx]=0u; else s_r57d_slot_hold[idx]=0u;
        }

'''
fn=fn.replace(release_anchor,release+release_anchor,1)
# Validation shares the burst FIFO to preserve hold order, but must not be
# stale-retired before its exact class comparison. Support both known worker
# spellings and fail rather than silently shipping an unprotected validator.
stale_patched=0
old_stale='if (stale_frame || stale_source) {'
if old_stale in fn:
    fn=fn.replace(old_stale,'if ((stale_frame || stale_source) && !(slot->selfcheck & 32u)) {',1)
    stale_patched=1
else:
    rx=re.compile(r'if \(burst65k && slot->u\.gbt65k\.frame_epoch != latest_epoch\) \{')
    mm=rx.search(fn)
    if mm:
        fn=fn[:mm.start()]+'if (burst65k && slot->u.gbt65k.frame_epoch != latest_epoch && !(slot->selfcheck & 32u)) {'+fn[mm.end():]
        stale_patched=1
if not stale_patched: fail('worker stale-retire condition not found for R57d validator exemption')

# Shared BG pending completion applies only to jobs without shadow hold.
fn=fn.replace('''            if (slot->selfcheck & 2u)\n                __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);''',
              '''            if ((slot->selfcheck & 2u) && !r57d_hold_seq)\n                __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);''')
if re.search(r'if \(slot->selfcheck & 2u\)\s*\n\s*__atomic_sub_fetch\(&s_bgsource_pending',fn):
    fail('compose worker still decrements shared BG pending for shadow-held GBT65K')
cs=cs[:a]+fn+cs[b:]

# ---- windraw declarations ----
# R57c has the first extern already.
ex='extern void tab5_guest_bus_post_raster(uint32_t vline); /* R57c CPU1->CPU0 ordered boundary */\n'
if ws.count(ex)!=1: fail('windraw R57c bus extern anchor mismatch')
ws=ws.replace(ex,ex+r'''extern uint32_t tab5_guest_bus_post_raster_hold(uint32_t vline);
extern void tab5_guest_bus_shadow_hold_cancel(uint32_t seq);
extern int tab5_compose_r57d_class_state(uint16_t key);
''',1)
# Extend extern compose signatures.
ws=add_sig_args(ws,'tab5_compose_submit_gbt65k_line','uint32_t r57d_shadow_seq, ')
ws=add_sig_args(ws,'tab5_compose_submit_gbt65k_exact_bt_line','uint16_t r57d_class, uint32_t r57d_shadow_seq, ')

# Add R57d locals next to R56s5 locals (function-wide because late exact submit needs them).
loc='    int r56s5_text_on = 0, r56s5_bg_on = 0;\n'
if ws.count(loc)!=1: fail('R56s5 locals anchor mismatch')
ws=ws.replace(loc,loc+r'''    uint16_t r57d_class = 0xffffu;
    int r57d_class_state = 3;
    uint32_t r57d_shadow_seq = 0u;
''',1)

# Replace only the quarantine's forced-safe line. Keep its useful one-time order trace.
old='            const int r56s5_exact_host = 0; /* correctness quarantine: no visible-TEXT CPU0 host-BT */'
if ws.count(old)!=1: fail('R56s5k forced-safe line mismatch')
new=r'''            /* R57d class key: gd + 8/16dot + BG0/BG1 enables + exact G/T/B priority.
             * A new class is rendered from stock CPU1 for 8 exact lines before
             * it may enter the CPU0 shadow path. */
            r57d_class=(uint16_t)(((stp && stp->gd)?1u:0u) |
                         ((stp && stp->chr_size==16u)?2u:0u) |
                         ((stp && (stp->reg9&1u))?4u:0u) |
                         ((stp && (stp->reg9&8u))?8u:0u) |
                         ((uint16_t)(grp_pri&3u)<<4) |
                         ((uint16_t)(text_pri&3u)<<6) |
                         ((uint16_t)(bg_pri&3u)<<8));
            r57d_class_state=tab5_compose_r57d_class_state(r57d_class);
            if (r56s3_text_visible && r57d_class_state!=2) {
                r56s4_65k_exact_bt=1;
                r56s5_text_src=text_src; r56s5_text_valid=text_valid;
                r56s5_text_on=text_on; r56s5_bg_on=bg_on;
                if (stp) { r56s5_bg_state=*stp; r56s5_have_bg_state=1; }
            }
            int r56s5_exact_host=0;
            if (r56s3_text_visible && r57d_class_state==2) {
                r57d_shadow_seq=bg_on ? tab5_guest_bus_post_raster_hold(VLINE) : 0u;
                r56s5_exact_host=(!bg_on || r57d_shadow_seq!=0u);
            } else if (!r56s3_text_visible && bg_on && stp && (!text_on || text_src)) {
                r57d_shadow_seq=tab5_guest_bus_post_raster_hold(VLINE);
            }'''
ws=ws.replace(old,new,1)

# Normal submit call: add hold sequence; require it whenever BG source is used.
callneedle='''                    r56s5_exact_host, grp_pri, bg_pri, text_pri,\n                    &RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket);'''
if ws.count(callneedle)!=1: fail('windraw normal R56s5 call anchor mismatch')
ws=ws.replace(callneedle,'''                    r56s5_exact_host, grp_pri, bg_pri, text_pri,\n                    &RenderBuf[VLINE * FULLSCREEN_WIDTH], r57d_shadow_seq, render_ticket);''',1)
# Admission must not use CPU0 BG source without a hold.
oldif='''            if ((!r56s3_text_visible || r56s5_exact_host) &&\n                (!text_on || text_src) && (!bg_on || stp)) {'''
if ws.count(oldif)!=1: fail('windraw early R56s5 admission anchor mismatch')
ws=ws.replace(oldif,'''            if ((!r56s3_text_visible || r56s5_exact_host) &&\n                (!text_on || text_src) && (!bg_on || (stp && r57d_shadow_seq))) {''',1)
# Cancel hold on rejected normal submission.
norm_after='''                    return; /* CPU0 owns write_end; no CPU1 frame-tail wait for 65K. */\n                }'''
if norm_after not in ws: fail('normal accepted return anchor missing')
ws=ws.replace(norm_after,norm_after+'''\n                if (r57d_shadow_seq) { tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq); r57d_shadow_seq=0u; }''',1)

# Late exact packet: pending class needs a frozen BG shadow for candidate validation.
late_anchor='''        const BG_HOST_LINE_STATE *r56s5_stp =\n            (r56s5_bg_on && r56s5_have_bg_state) ? &r56s5_bg_state : NULL;\n        int accepted = tab5_compose_submit_gbt65k_exact_bt_line('''
if ws.count(late_anchor)!=1: fail('late exact submit anchor mismatch')
ws=ws.replace(late_anchor,'''        const BG_HOST_LINE_STATE *r56s5_stp =\n            (r56s5_bg_on && r56s5_have_bg_state) ? &r56s5_bg_state : NULL;\n        if (r57d_class_state==0 && r56s5_bg_on && !r57d_shadow_seq)\n            r57d_shadow_seq=tab5_guest_bus_post_raster_hold(VLINE);\n        int accepted = tab5_compose_submit_gbt65k_exact_bt_line(''',1)
latecall='''            r56s5_bg_on, r56s5_text_on, grp_pri, bg_pri, text_pri,\n            &RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket);'''
if ws.count(latecall)!=1: fail('late exact call args anchor mismatch')
ws=ws.replace(latecall,'''            r56s5_bg_on, r56s5_text_on, grp_pri, bg_pri, text_pri,\n            &RenderBuf[VLINE * FULLSCREEN_WIDTH], r57d_class, r57d_shadow_seq, render_ticket);''',1)
# If late packet rejected, its hold must be cancelled before legacy fallback continues.
late_if='''        if (accepted > 0) {'''
# choose occurrence after exact function call
pos=ws.find('tab5_compose_submit_gbt65k_exact_bt_line(', ws.find(late_anchor.split('int accepted')[0]))
pos=ws.find(late_if,pos)
if pos<0: fail('late accepted if not found')
ws=ws[:pos]+'''        if (accepted <= 0 && r57d_shadow_seq) { tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq); r57d_shadow_seq=0u; }\n'''+ws[pos:]

# Main runtime marker.
ma='    ESP_LOGI(TAG, "PX68K_R57C: ordered raster-token boundaries ACTIVE; renderer ownership still correctness-fenced");\n'
if ms.count(ma)!=1: fail('R57c main marker anchor mismatch')
ms=ms.replace(ma,ma+f'    ESP_LOGI(TAG, "{MARK}");\n',1)

# The hot 65K shadow path must not participate in the old shared BG source
# pending barrier. Audit the normal submit function after all substitutions.
_a,_b=span(cs,'int tab5_compose_submit_gbt65k_line(')
_nf=cs[_a:_b]
if re.search(r'if \(bg_on\)\s*\n\s*__atomic_(?:add|sub)_fetch\(&s_bgsource_pending',_nf):
    fail('normal 65K submit still has an unguarded shared BG pending operation')

# Cross-file transaction audit BEFORE writes.
checks=[
 (TAG in bc,'bus R57d lineage'),
 ('tab5_guest_bus_post_raster_hold' in ws,'windraw ordered hold'),
 ('PX68K_R57D_CLASS_VALIDATE' in cs,'class validator'),
 ('R57D_CLASS_PASS_NEED 8u' in cs,'8-line certification'),
     ('const int burst65k = s_gbt65k_burst_ready;' in cs,'held validation shares burst FIFO'),
     ('!(slot->selfcheck & 32u)' in cs,'validation stale-retire exemption'),
 ('r57d_bg_src()[' in cs and 'r57d_c16_src()[' in cs,'CPU0 shadow source'),
 ('int tab5_compose_r57d_class_state' in cs,'class getter'),
 ('r57d_shadow_seq' in hs,'header hold contract'),
 (MARK in ms,'main runtime marker'),
 ('const int r56s5_exact_host = 0;' not in ws,'quarantine forced-off removed'),
 ('PX68K_R56S5K_VISIBLE_TEXT_QUARANTINE' in ws,'correctness lineage retained'),
]
for ok,msg in checks:
    if not ok: fail('audit: '+msg)

BUS_C.write_text(bc,encoding='utf-8',newline='')
BUS_H.write_text(bh,encoding='utf-8',newline='')
C.write_text(cs,encoding='utf-8',newline='')
H.write_text(hs,encoding='utf-8',newline='')
W.write_text(ws,encoding='utf-8',newline='')
M.write_text(ms,encoding='utf-8',newline='')
print('R57d3 BIG DETACH applied by function-block rewrite: ordered CPU0 BG/Sprite shadow source + 8-line per-class visible-TEXT certification; CPU1 never waits')
