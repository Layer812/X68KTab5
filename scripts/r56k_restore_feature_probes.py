#!/usr/bin/env python3
from pathlib import Path
import sys

P = Path('src/tab5_audio.cpp')
BASE = 'Build 6.12f Tab5 audio cold recovery'


def fail(msg, rc=2):
    print('R56k feature-probe restore ERROR:', msg, file=sys.stderr)
    sys.exit(rc)


def match_brace(text, open_pos):
    depth=0; i=open_pos; state='code'; quote=''
    while i < len(text):
        c=text[i]; n=text[i+1] if i+1 < len(text) else ''
        if state=='code':
            if c=='/' and n=='/': state='line'; i+=2; continue
            if c=='/' and n=='*': state='block'; i+=2; continue
            if c in ('"', "'"): state='str'; quote=c; i+=1; continue
            if c=='{': depth += 1
            elif c=='}':
                depth -= 1
                if depth==0: return i
            i+=1
        elif state=='line':
            if c=='\n': state='code'
            i+=1
        elif state=='block':
            if c=='*' and n=='/': state='code'; i+=2
            else: i+=1
        else:
            if c=='\\': i+=2
            elif c==quote: state='code'; i+=1
            else: i+=1
    raise RuntimeError('unbalanced braces')


def remove_comment_followed_block(text, comment_marker):
    st=text.find(comment_marker)
    if st < 0:
        return text, False
    # Include indentation/newline before comment when harmless.
    line_st=text.rfind('\n',0,st)+1
    op=text.find('{',st)
    if op < 0:
        fail('block opening brace not found after '+comment_marker, 4)
    en=match_brace(text,op)
    j=en+1
    while j < len(text) and text[j] in ' \t': j+=1
    if j < len(text) and text[j]=='\n': j+=1
    return text[:line_st] + text[j:], True


def main():
    if not P.is_file(): fail(f'{P} not found; run from G:\\px68k-tab5')
    raw=P.read_bytes(); nl='\r\n' if b'\r\n' in raw else '\n'
    s=raw.decode('utf-8', errors='strict').replace('\r\n','\n')
    if BASE not in s:
        fail('6.12f baseline anchor missing; refusing unknown audio source',3)

    changed=False
    # R56j lives entirely inside the R56i one-shot hardware-tone block, so
    # removing the outer R56i block also removes explicit ES8388/PI4IO writes.
    s,c=remove_comment_followed_block(s,'/* PX68K_AUDIO_R56I hardware tone probe.')
    changed |= c
    s,c=remove_comment_followed_block(s,'/* PX68K_AUDIO_R56I first nonzero egress amplitude proof. */')
    changed |= c

    latch='''/* PX68K_AUDIO_R56I: one-shot physical-egress/PCM-amplitude proof. */\nstatic bool s_r56i_egress_logged = false;\n'''
    if latch in s:
        s=s.replace(latch,'',1); changed=True

    bad=['PX68K_AUDIO_R56I','PX68K_AUDIO_R56J','r56j_es8388','r56j_hw_ok','s_r56i_egress_logged']
    left=[x for x in bad if x in s]
    if left:
        fail('probe restore incomplete, leftover: '+', '.join(left),5)

    P.write_bytes(s.replace('\n',nl).encode('utf-8'))
    print('R56k feature-probe restore:', 'removed R56i/j diagnostic modifications' if changed else 'already clean baseline', P)

if __name__=='__main__': main()
