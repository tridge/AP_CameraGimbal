#pragma once
#include <cctype>
#include <cstddef>
#include <cstring>
// Bounded reader shared with the Z1-Mini firmware manifest validator.
struct gcu_json { const char *p, *end; };
static inline void gcu_json_space(struct gcu_json *j)
{ while (j->p<j->end && strchr(" \t\r\n",*j->p) && *j->p) j->p++; }
static inline bool gcu_json_take(struct gcu_json *j, char c)
{ gcu_json_space(j); if (j->p==j->end || *j->p!=c) return false; j->p++; return true; }
static inline bool gcu_json_string(struct gcu_json *j, char *out, size_t capacity)
{
    if (!gcu_json_take(j,'"')) return false;
    size_t used=0;
    while (j->p<j->end) {
        unsigned char c=(unsigned char)*j->p++;
        if (c=='"') { if (out) out[used]=0; return true; }
        if (c<32) return false;
        if (c=='\\') {
            if (j->p==j->end) return false;
            c=(unsigned char)*j->p++;
            if (c=='u') {
                unsigned code=0;
                for (unsigned n=0;n<4;n++) {
                    if (j->p==j->end) return false;
                    unsigned char h=(unsigned char)*j->p++;
                    if (!isxdigit(h)) return false;
                    code=code*16+(h<='9' ? h-'0' : (h|32)-'a'+10);
                }
                /* Required keys/values are ASCII. Do not truncate Unicode or
                 * embedded NUL into a matching guard name. */
                if (out && (code==0 || code>127)) return false;
                c=(unsigned char)code;
            } else {
                const char *escapes="\"\\/bfnrt", *found=strchr(escapes,c);
                if (!c || !found) return false;
                c=(unsigned char)"\"\\/\b\f\n\r\t"[found-escapes];
            }
        }
        if (out) { if (used+1>=capacity) return false; out[used++]=(char)c; }
    }
    return false;
}
static inline bool gcu_json_literal(struct gcu_json *j, const char *value)
{
    gcu_json_space(j);
    size_t length=strlen(value);
    if ((size_t)(j->end-j->p)<length || memcmp(j->p,value,length)) return false;
    j->p+=length;
    return true;
}
static inline bool gcu_json_value(struct gcu_json *j, unsigned depth)
{
    gcu_json_space(j);
    if (depth>16 || j->p==j->end) return false;
    char c=*j->p;
    if (c=='"') return gcu_json_string(j,NULL,0);
    if (c=='{' || c=='[') {
        j->p++;
        char close=c=='{' ? '}' : ']';
        if (gcu_json_take(j,close)) return true;
        do {
            if (c=='{' && (!gcu_json_string(j,NULL,0) || !gcu_json_take(j,':'))) return false;
            if (!gcu_json_value(j,depth+1)) return false;
            if (gcu_json_take(j,close)) return true;
        } while (gcu_json_take(j,','));
        return false;
    }
    if (c=='t') return gcu_json_literal(j,"true");
    if (c=='f') return gcu_json_literal(j,"false");
    if (c=='n') return gcu_json_literal(j,"null");
    if (*j->p=='-') j->p++;
    if (j->p==j->end || !isdigit((unsigned char)*j->p)) return false;
    if (*j->p=='0') j->p++;
    else while (j->p<j->end && isdigit((unsigned char)*j->p)) j->p++;
    if (j->p<j->end && *j->p=='.') {
        j->p++;
        if (j->p==j->end || !isdigit((unsigned char)*j->p)) return false;
        while (j->p<j->end && isdigit((unsigned char)*j->p)) j->p++;
    }
    if (j->p<j->end && (*j->p=='e' || *j->p=='E')) {
        j->p++;
        if (j->p<j->end && (*j->p=='+' || *j->p=='-')) j->p++;
        if (j->p==j->end || !isdigit((unsigned char)*j->p)) return false;
        while (j->p<j->end && isdigit((unsigned char)*j->p)) j->p++;
    }
    return true;
}
