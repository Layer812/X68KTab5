from pathlib import Path
import re

ROOT=Path('.')
BUS_C=ROOT/'src/tab5_guest_bus.c'
BUS_H=ROOT/'src/tab5_guest_bus.h'
W=ROOT/'components/px68k/libretro/windraw.c'
BG=ROOT/'components/px68k/x68k/bg.c'
MAIN=ROOT/'src/main.c'
TAG='PX68K_R57C_RASTER_TOKEN'
MARK='PX68K_R57C: ordered raster-token boundaries ACTIVE; renderer ownership still correctness-fenced'

def fail(msg):
    print('R57c patch ERROR:',msg)
    raise SystemExit(2)

for p in (BUS_C,BUS_H,W,BG,MAIN):
    if not p.is_file(): fail(f'missing {p}')

bc=BUS_C.read_text(encoding='utf-8')
bh=BUS_H.read_text(encoding='utf-8')
w=W.read_text(encoding='utf-8')
bg=BG.read_text(encoding='utf-8')
ma=MAIN.read_text(encoding='utf-8')

if 'PX68K_R57B_BG_SHADOW' not in bc or 'PX68K_R57B:' not in ma:
    fail('R57b lineage missing; apply R57b first')
if 'PX68K_R57A2_DEDICATED_CONSUMER' not in bc:
    fail('R57a2 dedicated consumer missing')

# Idempotent verification.
if TAG in bc and MARK in ma and 'TAB5_R57_DOMAIN_RASTER' in bh and 'tab5_guest_bus_post_raster' in w:
    print('R57c already applied and verified')
    raise SystemExit(0)
if TAG in bc or MARK in ma or 'TAB5_R57_DOMAIN_RASTER' in bh or 'tab5_guest_bus_post_raster' in w:
    fail('partial R57c state')

# Header: one new ordered event kind and a narrow one-way API.
enum_anchor='    TAB5_R57_DOMAIN_BG_RESET = 5,\n'
if bh.count(enum_anchor)!=1: fail(f'header BG_RESET anchor count={bh.count(enum_anchor)}')
bh=bh.replace(enum_anchor,enum_anchor+'    TAB5_R57_DOMAIN_RASTER   = 6,\n',1)
proto='void tab5_guest_bus_post_write(uint32_t domain, uint32_t address, uint8_t value);\n'
if bh.count(proto)!=1: fail(f'header post_write prototype count={bh.count(proto)}')
bh=bh.replace(proto,proto+'void tab5_guest_bus_post_raster(uint32_t vline);\n',1)

# Source lineage comment.
comment=' * PX68K_R57B_BG_SHADOW\n'
if bc.count(comment)!=1: fail('bus lineage comment anchor mismatch')
bc=bc.replace(comment,comment+' * PX68K_R57C_RASTER_TOKEN\n',1)

# Telemetry state: tokens are ordinary ordered journal records, so order/hash/
# overflow accounting automatically covers them as well.
ctr_anchor='static volatile uint32_t s_shadow_reset, s_shadow_chr8_w, s_shadow_chr16_w;\n'
if bc.count(ctr_anchor)!=1: fail('raster telemetry declaration anchor mismatch')
bc=bc.replace(ctr_anchor,ctr_anchor+
'''static volatile uint32_t s_raster_tokens, s_last_raster_seq, s_last_raster_vline;\n''',1)

# Apply raster boundary after all older writes at that sequence point. It does
# not mutate shadow data; it records the exact shadow sequence represented by
# the token. Future R57d work may pause the consumer at this point while CPU0
# renders from the frozen shadow, without stalling CPU1.
old_reset='''    } else if (domain==TAB5_R57_DOMAIN_BG_RESET) {\n        shadow_zero_bg();\n    }\n'''
new_reset='''    } else if (domain==TAB5_R57_DOMAIN_BG_RESET) {\n        shadow_zero_bg();\n    } else if (domain==TAB5_R57_DOMAIN_RASTER) {\n        __atomic_add_fetch(&s_raster_tokens,1u,__ATOMIC_RELAXED);\n        __atomic_store_n(&s_last_raster_seq,e->seq,__ATOMIC_RELAXED);\n        __atomic_store_n(&s_last_raster_vline,e->addr,__ATOMIC_RELAXED);\n    }\n'''
if bc.count(old_reset)!=1: fail(f'shadow_apply reset anchor count={bc.count(old_reset)}')
bc=bc.replace(old_reset,new_reset,1)

# Reset new counters on init.
init_anchor='    s_shadow_sprite_w=s_shadow_bgreg_w=s_shadow_bg_w=s_shadow_reset=s_shadow_chr8_w=s_shadow_chr16_w=0u;\n'
if bc.count(init_anchor)!=1: fail('bus init counter anchor mismatch')
bc=bc.replace(init_anchor,init_anchor+'    s_raster_tokens=s_last_raster_seq=s_last_raster_vline=0u;\n',1)

# Add API after the producer function. Brace-scope the function so we never
# depend on the following function name/order.
def function_span(text, signature):
    p=text.find(signature)
    if p<0: fail(f'function not found: {signature}')
    b=text.find('{',p)
    if b<0: fail(f'opening brace not found: {signature}')
    depth=0; i=b; in_s=in_c=in_line=in_block=False; esc=False
    while i<len(text):
        ch=text[i]; nx=text[i+1] if i+1<len(text) else ''
        if in_line:
            if ch=='\n': in_line=False
        elif in_block:
            if ch=='*' and nx=='/': in_block=False; i+=1
        elif in_s:
            if esc: esc=False
            elif ch=='\\': esc=True
            elif ch=='"': in_s=False
        elif in_c:
            if esc: esc=False
            elif ch=='\\': esc=True
            elif ch=="'": in_c=False
        else:
            if ch=='/' and nx=='/': in_line=True; i+=1
            elif ch=='/' and nx=='*': in_block=True; i+=1
            elif ch=='"': in_s=True
            elif ch=="'": in_c=True
            elif ch=='{': depth+=1
            elif ch=='}':
                depth-=1
                if depth==0: return p,i+1
        i+=1
    fail(f'unclosed function: {signature}')

a,b=function_span(bc,'void tab5_guest_bus_post_write(')
raster_api='''\n\nvoid tab5_guest_bus_post_raster(uint32_t vline)\n{\n    /* A raster token is just another SPSC record. CPU1 never waits here. */\n    tab5_guest_bus_post_write(TAB5_R57_DOMAIN_RASTER,vline,0u);\n}\n'''
bc=bc[:b]+raster_api+bc[b:]

# Enrich report without changing its gate. attempt==prod==cons and matching
# hashes now prove both write events and raster boundaries were lossless.
fmt_old='''maxDepth=%lu shadow{spr=%lu reg=%lu bg=%lu c8=%lu c16=%lu reset=%lu}"'''
fmt_new='''maxDepth=%lu raster=%lu lastRaster={seq=%lu y=%lu} shadow{spr=%lu reg=%lu bg=%lu c8=%lu c16=%lu reset=%lu}"'''
if bc.count(fmt_old)!=1: fail(f'report format anchor count={bc.count(fmt_old)}')
bc=bc.replace(fmt_old,fmt_new,1)
args_old='''             (unsigned long)ov,(unsigned long)of,(unsigned long)depth,(unsigned long)ld_relaxed(&s_max_depth),\n             (unsigned long)ld_relaxed(&s_shadow_sprite_w),(unsigned long)ld_relaxed(&s_shadow_bgreg_w),'''
args_new='''             (unsigned long)ov,(unsigned long)of,(unsigned long)depth,(unsigned long)ld_relaxed(&s_max_depth),\n             (unsigned long)ld_relaxed(&s_raster_tokens),(unsigned long)ld_relaxed(&s_last_raster_seq),\n             (unsigned long)ld_relaxed(&s_last_raster_vline),\n             (unsigned long)ld_relaxed(&s_shadow_sprite_w),(unsigned long)ld_relaxed(&s_shadow_bgreg_w),'''
if bc.count(args_old)!=1: fail(f'report argument anchor count={bc.count(args_old)}')
bc=bc.replace(args_old,args_new,1)

# Windraw: declare the one-way post function next to existing ESP compositor
# declarations. Then post immediately after the VLINE==-1 early return. This
# point is after all guest writes preceding the scanline and before any host
# raster work, so it is the exact ownership boundary we need.
extern_anchor='extern int tab5_compose_gbt65k_line_admit(uint32_t y, uint32_t height);\n'
if w.count(extern_anchor)!=1: fail(f'windraw compositor extern anchor count={w.count(extern_anchor)}')
w=w.replace(extern_anchor,'extern void tab5_guest_bus_post_raster(uint32_t vline); /* R57c CPU1->CPU0 ordered boundary */\n'+extern_anchor,1)

pat=re.compile(r'(if\s*\(\s*VLINE\s*==\s*\(uint32_t\)-1\s*\)\s*\{\s*return\s*;\s*\})')
m=pat.search(w)
if not m: fail('windraw VLINE early-return block not found')
insert='''\n\n    /* PX68K_R57C_RASTER_TOKEN: posted write only; CPU1 never waits. */\n    tab5_guest_bus_post_raster(VLINE);'''
w=w[:m.end()]+insert+w[m.end():]

# Main lineage.
main_anchor='    ESP_LOGI(TAG, "PX68K_R57B: CPU0 BG/Sprite render-shadow mirror ACTIVE; ownership switch deferred to raster-token phase");\n'
if ma.count(main_anchor)!=1: fail(f'main R57b anchor count={ma.count(main_anchor)}')
ma=ma.replace(main_anchor,main_anchor+f'    ESP_LOGI(TAG, "{MARK}");\n',1)

BUS_H.write_text(bh,encoding='utf-8',newline='')
BUS_C.write_text(bc,encoding='utf-8',newline='')
W.write_text(w,encoding='utf-8',newline='')
MAIN.write_text(ma,encoding='utf-8',newline='')
print('R57c applied: lossless ordered raster tokens added to R57b CPU0 shadow journal; renderer/barriers remain correctness-fenced')
