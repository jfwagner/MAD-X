/*
 o---------------------------------------------------------------------o
 |
 | Numdiff
 |
 | Copyright (c) 2012+ laurent.deniau@cern.ch
 | Gnu General Public License
 |
 o---------------------------------------------------------------------o
  
   Purpose:
     create constraints content
     print, scan constraints from file
 
 o---------------------------------------------------------------------o
*/

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>

#include "args.h"
#include "utils.h"
#include "error.h"
#include "register.h"
#include "constraint.h"

#define T struct constraint
#define S struct slice

// ----- private

static void
printSlc(const S *s, FILE *out)
{
  if (slice_isFull(s)) {
    putc('*', out);
    return;
  }

  fprintf(out, "%u", slice_first(s));

  if (slice_isUnit(s)) return;

  if (slice_isInfinite(s)) {
    putc('-', out);
    putc('$', out);
  } else
   fprintf(out, "-%u", slice_last(s));

  if (slice_stride(s) != 1)
    fprintf(out, "/%u", slice_stride(s));    
}

static int
readSlcOrRng(S *s, FILE *in)
{
  int c, r = 1;
  uint first=0, last=0, stride=1;

  // skip spaces
  while((c = getc(in)) != EOF && isblank(c)) ;
  if (c == EOF) return EOF;

  // ('*'|num)
  if (c == '*') { last = UINT_MAX; goto finish; }
  else {
    ungetc(c, in);
    if (fscanf(in, "%u", &first) != 1) return EOF;
  }

  // (':'|'-')?
  c = getc(in);
       if (c == ':') r = 0;  // slice
  else if (c == '-') ;       // range
  else { ungetc(c, in); last = first; goto finish; }

  // ('$'|num)
  c = getc(in);
  if (c == '$') last = UINT_MAX;
  else {
    ungetc(c, in);
    if (fscanf(in, "%u", &last) != 1) return EOF;
  }

  // ('/'num)? 
  c = getc(in);
  if (c != '/') { ungetc(c, in); stride = 1; goto finish; }
  else
    if (fscanf(in, "%u", &stride) != 1) return EOF;

finish:
  if (r)
    *s = slice_initLastStride(first, last, stride);
  else
    *s = slice_initSizeStride(first, last, stride);

  trace("<-readSlcOrRng %u%c%u/%u", first, r ? '-' : ':', last, stride);

  return 0;
}

#define EPS_INVALID \
{ \
  cmd = eps_invalid; \
  trace("[%d] invalid '%s', buf='%s'", row, str, buf); \
  break; \
}


static int
readEps(struct eps *e, FILE *in, int row)
{
  static const char *const op[] = { "", "-", "/", "-/" };
  int c = 0, n = 0, cmd = eps_invalid;
  char str[16], buf[TAG_LEN_2], *end;

  while (1) {
    // parse next command
    *str = *buf = 0;
    n = fscanf(in, "%*[ \t]%16[^= \t\n\r!#]", str);
    str[sizeof str-1]=0;

    if (n == EOF || *str == 0) break;

// commands without '='
    if ((c = getc(in)) != '=') {
      ungetc(c, in);

           if (strcmp(str, "skip") == 0) {
        cmd |= eps_skip;  trace("[%d] skip", row);
      }
      else if (strcmp(str, "ign") == 0) {
        cmd |= eps_ign;   trace("[%d] ign", row);
      }
      else if (strcmp(str, "istr") == 0) {
        cmd |= eps_istr;  trace("[%d] istr", row);
      }
      else if (strcmp(str, "equ") == 0) {
        cmd |= eps_equ;   trace("[%d] equ", row);
      }
      else if (strcmp(str, "any") == 0) {
        cmd |= eps_any;   trace("[%d] any", row);
      }
      else if (strcmp(str, "all") == 0) {
        cmd &= ~eps_any;  trace("[%d] all", row);
      }
      else if (strcmp(str, "large") == 0) {
        cmd |= eps_large; trace("[%d] large", row);
      }
      else if (strcmp(str, "small") == 0) {
        cmd &= ~eps_large; trace("[%d] small", row);
      }
      else if (strcmp(str, "trace") == 0) {
        cmd |= eps_trace; trace("[%d] trace", row);
      }
      else EPS_INVALID;

// commands with tag [='...'] or [="..."]
    } else if ((c = ungetc(getc(in), in)) == '\'' || c == '"') {
      char c2 = 0;

      if (strcmp(str, "omit") == 0 && (n = fscanf(in, "%*['\"]%" MkString(TAG_LEN_2) "[^'\"]%c", e->tag, &c2)) == 2) {
        e->tag[sizeof e->tag-1] = 0;
        cmd |= eps_omit | eps_equ;
        trace("[%d] omit='%s'", row, e->tag);
        ensure(*e->tag, "invalid empty tag (%s:%d)", option.cfg_file, row);
        ensure(c == c2, "invalid tag quotes (%s:%d)", option.cfg_file, row);
        ensure(!(cmd & (eps_goto | eps_gonum)), "omit tag conflicting with goto (%s:%d)", option.cfg_file, row);
      }
      else if (strcmp(str, "goto") == 0 && (n = fscanf(in, "%*['\"]%" MkString(TAG_LEN_2) "[^'\"]%c", e->tag, &c2)) == 2) {
        e->tag[sizeof e->tag-1] = 0;
        e->num = strtod(e->tag, &end);
        cmd |= !*end ? eps_gonum | eps_istr : eps_goto;
        trace("[%d] goto='%s'%s", row, e->tag, cmd & eps_gonum ? " (num)" : "");
        ensure(*e->tag, "invalid empty tag (%s:%d)", option.cfg_file, row);
        ensure(c == c2, "invalid tag quotes (%s:%d)", option.cfg_file, row);
        ensure(!(cmd & eps_omit), "goto tag conflicting with omit (%s:%d)", option.cfg_file, row);
      }
      else EPS_INVALID;
    }

// commands with [=...]
    else {
      *buf = 0;
      n = fscanf(in, "%" MkString(TAG_LEN_2) "[^ \t\n\r!#]", buf);
      buf[sizeof buf-1]=0;

      ensure(n == 1 && *buf, "invalid assignment '%s' (%s:%d)", buf, option.cfg_file, row);

      // --- save registers
      if (*str == 'R' && !strchr(buf,'R')) {
        int rn = strtoul(str+1, &end, 10);
        ensure(reg_isvalid(rn) && !*end, "invalid register number %d (%s:%d)", rn, option.cfg_file, row);

             if (strcmp(buf, "lhs") == 0) {
           cmd |= eps_slhs;  e->slhs_reg = rn;  trace("[%d] R%d=lhs", row, e->slhs_reg);
        }
        else if (strcmp(buf, "rhs") == 0) {
           cmd |= eps_srhs;  e->srhs_reg = rn;  trace("[%d] R%d=rhs", row, e->srhs_reg);
        }
        else if (strcmp(buf, "dif") == 0) {
           cmd |= eps_sdif;  e->sdif_reg = rn;  trace("[%d] R%d=dif", row, e->sdif_reg);
        }
        else if (strcmp(buf, "err") == 0) {
           cmd |= eps_serr;  e->serr_reg = rn;  trace("[%d] R%d=err", row, e->serr_reg);
        }
        else if (strcmp(buf, "abs") == 0) {
           cmd |= eps_sabs;  e->sabs_reg = rn;  trace("[%d] R%d=abs", row, e->sabs_reg);
        }
        else if (strcmp(buf, "rel") == 0) {
           cmd |= eps_srel;  e->srel_reg = rn;  trace("[%d] R%d=rel", row, e->srel_reg);
        }
        else if (strcmp(buf, "dig") == 0) {
           cmd |= eps_sdig;  e->sdig_reg = rn;  trace("[%d] R%d=dig", row, e->sdig_reg);
        }
        else EPS_INVALID;
      }

      // --- move registers
      else if (*str == 'R' && strchr(buf,'R')) {
        ensure(e->reg_n < 5, "rule has too many moves (%s:%d)", option.cfg_file, row);

        int rn = strtoul(str+1, &end, 10);
        ensure(reg_isvalid(rn) && !*end, "invalid register reference '%s' (%s:%d)", str, option.cfg_file, row);

        int rn1=0, rn2=0;
        int s = *buf == '-' || (*buf && buf[1] == '-');
        int i = *buf == '/' || (*buf && buf[1] == '/');
        int n = sscanf(buf+s+i, "R%d..R%d", &rn1, &rn2);
        ensure(reg_isvalid(rn1) && (n == 1 || (reg_isvalid(rn2) && rn1 < rn2)),
               "invalid register reference(s) '%s' (%s:%d)", buf, option.cfg_file, row);

        // move single register
        if (n == 1) {
          cmd |= eps_move; trace("[%d] R%d=R%d", row, rn, rn1);
          e->dst_reg[e->reg_n] = rn; e->src_reg[e->reg_n] = reg_encode(rn1, s, i); e->cnt_reg[e->reg_n] = 1; e->reg_n++;
        }

        // move sequence of registers
        else if (n == 2) {
          cmd |= eps_move; trace("[%d] R%d=R%d..R%d", row, rn, rn1, rn2);
          e->dst_reg[e->reg_n] = rn; e->src_reg[e->reg_n] = reg_encode(rn1, s, i); e->cnt_reg[e->reg_n] = rn2-rn1+1; e->reg_n++;
        }
        else EPS_INVALID;
      }

      // --- load registers
      else if (*str != 'R' && strchr(buf,'R')) {
        int s = *buf == '-' || (*buf && buf[1] == '-');
        int i = *buf == '/' || (*buf && buf[1] == '/');
        int rn = strtoul(buf+s+i, &end, 10);
        ensure(reg_isvalid(rn) && !*end, "invalid register reference '%s' (%s:%d)", buf, option.cfg_file, row);

             if (strcmp(str, "lhs") == 0) {
           cmd |= eps_lhs;  e->lhs_reg = reg_encode(rn, s, i);  trace("[%d] lhs=%sR%d", row, op[s+2*i], rn);
        }
        else if (strcmp(str, "rhs") == 0) {
           cmd |= eps_rhs;  e->rhs_reg = reg_encode(rn, s, i);  trace("[%d] rhs=%sR%d", row, op[s+2*i], rn);
        }
        else if (strcmp(str, "goto") == 0) {
          cmd |= eps_gonum | eps_istr;
                            e->gto_reg = reg_encode(rn, s, i);  trace("[%d] goto=%sR%d", row, op[s+2*i], rn);
        }
        else if (strcmp(str, "scl") == 0) {
                            e->scl_reg = reg_encode(rn, s, i);  trace("[%d] scl=%sR%d", row, op[s+2*i], rn);
        }
        else if (strcmp(str, "off") == 0) {
                            e->off_reg = reg_encode(rn, s, i);  trace("[%d] off=%sR%d", row, op[s+2*i], rn);
        }
        else if (strcmp(str, "abs") == 0) {
          cmd |= eps_abs;   e->abs_reg = reg_encode(rn, s, i);  trace("[%d] abs=%sR%d", row, op[s+2*i], rn);  e->_abs_reg = reg_encode(rn, -s, i);
          ensure(e->abs >= 0.0 && (cmd & eps_large || e->abs <= 1.0), "invalid absolute constraint (%s:%d)", option.cfg_file, row);
        }
        else if (strcmp(str, "-abs") == 0) {
          cmd |= eps_abs;   e->_abs_reg = reg_encode(rn, s, i);  trace("[%d] -abs=%sR%d", row, op[s+2*i], rn);
          ensure(e->_abs <= 0.0 && (cmd & eps_large || e->_abs >= -1.0), "invalid absolute constraint (%s:%d)", option.cfg_file, row);
        }
        else if (strcmp(str, "rel") == 0) {
          cmd |= eps_rel;   e->rel_reg = reg_encode(rn, s, i);  trace("[%d] rel=%sR%d", row, op[s+2*i], rn);  e->_rel = reg_encode(rn, -s, i);
          ensure(e->rel >= 0.0 && (cmd & eps_large || e->rel <= 1.0), "invalid relative constraint (%s:%d)", option.cfg_file, row);
        }
        else if (strcmp(str, "-rel") == 0) {
          cmd |= eps_rel;   e->_rel_reg = reg_encode(rn, s, i);  trace("[%d] -rel=%sR%d", row, op[s+2*i], rn);
          ensure(e->_rel <= 0.0 && (cmd & eps_large || e->_rel >= -1.0), "invalid relative constraint (%s:%d)", option.cfg_file, row);
        }
        else if (strcmp(str, "dig") == 0) {
          cmd |= eps_dig;   e->dig_reg = reg_encode(rn, s, i);  trace("[%d] dig=%sR%d", row, op[s+2*i], rn);  e->_dig = reg_encode(rn, -s, i);
          ensure(e->dig >= 1.0, "invalid digital error (%s:%d)", option.cfg_file, row);
        }
        else if (strcmp(str, "-dig") == 0) {
          cmd |= eps_dig;   e->_dig_reg = reg_encode(rn, s, i);  trace("[%d] -dig=%sR%d", row, op[s+2*i], rn);
          ensure(e->_dig <= -1.0, "invalid digital error (%s:%d)", option.cfg_file, row);
        }
        else EPS_INVALID;
      }

      // --- load numbers
      else if (*str != 'R' && !strchr(buf,'R')) {
        double val = strtod(buf, &end);
        ensure(!*end, "invalid number '%s'", buf, option.cfg_file, row);

             if (strcmp(str, "lhs") == 0) {
           cmd |= eps_lhs;  e->lhs = val;  trace("[%d] lhs=%g", row, val);
        }
        else if (strcmp(str, "rhs") == 0) {
           cmd |= eps_rhs;  e->rhs = val;  trace("[%d] rhs=%g", row, val);
        }

        else if (strcmp(str, "scl") == 0) {
                            e->scl = val;  trace("[%d] scl=%g", row, val);
        }
        else if (strcmp(str, "off") == 0) {
                            e->off = val;  trace("[%d] off=%g", row, val);
        }

        else if (strcmp(str, "abs") == 0) {
          cmd |= eps_abs;   e->abs = val;  trace("[%d] abs=%g", row, val);  e->_abs = -val;
          ensure(e->abs >= 0.0 && (cmd & eps_large || e->abs <= 1.0), "invalid absolute constraint (%s:%d)", option.cfg_file, row);
        }
        else if (strcmp(str, "-abs") == 0) {
          cmd |= eps_abs;   e->_abs = val;  trace("[%d] -abs=%g", row, val);
          ensure(e->_abs <= 0.0 && (cmd & eps_large || e->_abs >= -1.0), "invalid absolute constraint (%s:%d)", option.cfg_file, row);
        }

        else if (strcmp(str, "rel") == 0) {
          cmd |= eps_rel;   e->rel = val;  trace("[%d] rel=%g", row, val);  e->_rel = -val;
          ensure(e->rel >= 0.0 && (cmd & eps_large || e->rel <= 1.0), "invalid relative constraint (%s:%d)", option.cfg_file, row);
        }
        else if (strcmp(str, "-rel") == 0) {
          cmd |= eps_rel;   e->_rel = val;  trace("[%d] -rel=%g", row, val);
          ensure(e->_rel <= 0.0 && (cmd & eps_large || e->_rel >= -1.0), "invalid relative constraint (%s:%d)", option.cfg_file, row);
        }

        else if (strcmp(str, "dig") == 0) {
          cmd |= eps_dig;   e->dig = val;  trace("[%d] dig=%g", row, val);  e->_dig = -val;
          ensure(e->dig >= 1.0, "invalid digital error (%s:%d)", option.cfg_file, row);
        }
        else if (strcmp(str, "-dig") == 0) {
          cmd |= eps_dig;   e->_dig = val;  trace("[%d] -dig=%g", row, val);
          ensure(e->_dig <= -1.0, "invalid digital error (%s:%d)", option.cfg_file, row);
        }
        else EPS_INVALID;
      }
      else EPS_INVALID;
    }

    // next char
    ungetc((c = getc(in)), in);
    if (c == EOF || (isspace(c) && !isblank(c)) || c == '#' || c == '!') break; 
  }

  // cleanup non-persistant flags (e.g. large)
  e->cmd = (enum eps_cmd)(cmd & eps_mask);  // cast needed because of icc spurious warnings

  trace("<-readEps cmd = %d, str = '%s', c = '%c'", cmd, str, c);

  return cmd == eps_invalid || n == EOF ? EOF : 0;
}

// ----- interface

void
constraint_print(const T* cst, FILE *out)
{
  static const char *const op[] = { "", "-", "/", "-/" };
  int rn, s, i;

  if (!out) out = stdout;
  if (!cst) { fprintf(out, "(null)"); return; }

  printSlc(&cst->row, out);
  putc(' ', out);
  printSlc(&cst->col, out);
  putc(' ', out);

  if (cst->eps.cmd & eps_any)    fprintf(out, "any ");
  if (cst->eps.cmd & eps_equ)    fprintf(out, "equ ");
  if (cst->eps.cmd & eps_ign)    fprintf(out, "ign ");
  if (cst->eps.cmd & eps_istr)   fprintf(out, "istr ");
  if (cst->eps.cmd & eps_skip)   fprintf(out, "skip ");
  if (cst->eps.cmd & eps_trace)  fprintf(out, "trace ");

  if (cst->eps.cmd & eps_omit)   fprintf(out, "omit='%s' ", cst->eps.tag);
  if (cst->eps.cmd & eps_goto)   fprintf(out, "goto='%s' ", cst->eps.tag);

// --- loads
  if (cst->eps.cmd & eps_gonum)  {
    if (cst->eps.gto_reg) {
      rn = reg_decode(cst->eps.gto_reg, &s, &i);
      fprintf(out, "goto=%sR%d (num) ", op[s+2*i], rn);
    }
    else fprintf(out, "goto='%s' (num) ", cst->eps.tag);
  }

  if (cst->eps.cmd & eps_lhs) {
    if (cst->eps.lhs_reg) {
      rn = reg_decode(cst->eps.lhs_reg, &s, &i);
      fprintf(out, "lhs=%sR%d ", op[s+2*i], rn);
    }
    else fprintf(out, "lhs=%g ", cst->eps.lhs);
  }

  if (cst->eps.cmd & eps_rhs) {
    if (cst->eps.rhs_reg) {
      rn = reg_decode(cst->eps.rhs_reg, &s, &i);
      fprintf(out, "rhs=%sR%d ", op[s+2*i], rn);
    }
    else fprintf(out, "rhs=%g ", cst->eps.rhs);
  }

  if (cst->eps.scl != 1.0) fprintf(out, "scl=%g ", cst->eps.scl);
  if (cst->eps.scl_reg) {
    rn = reg_decode(cst->eps.scl_reg, &s, &i);
    fprintf(out, "scl=%sR%d ", op[s+2*i], rn);
  }
  else fprintf(out, "scl=%g ", cst->eps.scl);

  if (cst->eps.off != 1.0) fprintf(out, "off=%g ", cst->eps.off);
  if (cst->eps.off_reg) {
    rn = reg_decode(cst->eps.off_reg, &s, &i);
    fprintf(out, "off=%sR%d ", op[s+2*i], rn);
  }
  else fprintf(out, "off=%g ", cst->eps.off);

  if (cst->eps.cmd & eps_abs) {
    if (cst->eps.abs_reg) {
      rn = reg_decode(cst->eps.abs_reg, &s, &i);
      fprintf(out, "abs=%sR%d ", op[s+2*i], rn);
    }
    else fprintf(out, cst->eps.abs == DBL_MIN ? "abs=eps " : "%sabs=%g ",
                      cst->eps.abs > 1        ? "large " : "", cst->eps.abs);

    if (cst->eps._abs_reg && cst->eps._abs_reg != -cst->eps.abs_reg) {
      rn = reg_decode(cst->eps._abs_reg, &s, &i);
      fprintf(out, "-abs=%sR%d ", op[s+2*i], rn);
    }
    else if (cst->eps._abs != -cst->eps.abs)
        fprintf(out, cst->eps._abs == -DBL_MIN ? "-abs=-eps " : "%s-abs=%g ",
                     cst->eps._abs < -1        ? "large " : "", cst->eps._abs);
  }

  if (cst->eps.cmd & eps_rel) {
    if (cst->eps.rel_reg) {
      rn = reg_decode(cst->eps.rel_reg, &s, &i);
      fprintf(out, "rel=%sR%d ", op[s+2*i], rn);
    }
    else fprintf(out, "%srel=%g ", cst->eps.rel > 1 ? "large " : "", cst->eps.rel);

    if (cst->eps._rel_reg && cst->eps._rel_reg != -cst->eps.rel_reg) {
      rn = reg_decode(cst->eps._rel_reg, &s, &i);
      fprintf(out, "-rel=%sR%d ", op[s+2*i], rn);
    }
    else if (cst->eps._rel != -cst->eps.rel)
      fprintf(out, "%s-rel=%g ", cst->eps._rel < -1 ? "large " : "", cst->eps._rel);
  }

  if (cst->eps.cmd & eps_dig) {
    if (cst->eps.dig_reg) {
      rn = reg_decode(cst->eps.dig_reg, &s, &i);
      fprintf(out, "dig=%sR%d ", op[s+2*i], rn);
    }
    else fprintf(out, "dig=%g ", cst->eps.dig);

    if (cst->eps._dig_reg && cst->eps._dig_reg != -cst->eps.dig_reg) {
      rn = reg_decode(cst->eps._dig_reg, &s, &i);
      fprintf(out, "-dig=%sR%d ", op[s+2*i], rn);
    }
    else if (cst->eps._dig != -cst->eps.dig)
      fprintf(out, "-dig=%g ", cst->eps._dig);
  }   

// --- moves
  if (cst->eps.cmd & eps_move) {
    for (int i=0; i < cst->eps.reg_n; i++) {
      rn = reg_decode(cst->eps.src_reg[i], &s, &i);
      if (cst->eps.cnt_reg[i] > 1)
        fprintf(out, "R%d=%sR%d..R%d ", cst->eps.dst_reg[i], op[s+2*i], rn, cst->eps.src_reg[i]+cst->eps.cnt_reg[i]-1);
      else
        fprintf(out, "R%d=%sR%d ", cst->eps.dst_reg[i], op[s+2*i], rn);
    }
  }

// --- saves
  if (cst->eps.cmd & eps_slhs)  fprintf(out, "R%d=lhs ", cst->eps.slhs_reg);
  if (cst->eps.cmd & eps_srhs)  fprintf(out, "R%d=rhs ", cst->eps.srhs_reg);
  if (cst->eps.cmd & eps_sdif)  fprintf(out, "R%d=dif ", cst->eps.sdif_reg);
  if (cst->eps.cmd & eps_serr)  fprintf(out, "R%d=err ", cst->eps.serr_reg);
  if (cst->eps.cmd & eps_sabs)  fprintf(out, "R%d=abs ", cst->eps.sabs_reg);
  if (cst->eps.cmd & eps_srel)  fprintf(out, "R%d=rel ", cst->eps.srel_reg);
  if (cst->eps.cmd & eps_sdig)  fprintf(out, "R%d=dig ", cst->eps.sdig_reg);
}

void
constraint_scan(T* cst, FILE *in, int *row)
{
  int c;
  assert(cst && row);

  *cst = (T){ .eps = { .cmd = eps_invalid, .scl=1.0 } };

  if (!in) in = stdin;

retry:

  while((c = getc(in)) != EOF && isblank(c)) ;

  // end of file
  if (c == EOF) return;

  ungetc(c, in);

  // comment or empty line
  if (c == '\n' || c == '\r' || c == '#' || c == '!') {
    if (skipLine(in, 0) == '\n') ++*row;
    goto retry;
  }

  cst->idx  = -1;
  cst->line = *row;

  ensure(readSlcOrRng(&cst->row, in      ) != EOF, "invalid row range (%s:%d)"   , option.cfg_file, *row);
  ensure(readSlcOrRng(&cst->col, in      ) != EOF, "invalid column range (%s:%d)", option.cfg_file, *row);
  ensure(readEps     (&cst->eps, in, *row) != EOF, "invalid constraint or command (%s:%d)", option.cfg_file, *row);

  // expand to all columns
  if (cst->eps.cmd & eps_skip || cst->eps.cmd & eps_goto)
    cst->col = slice_initAll();

  // adjust row count
  if (skipLine(in, 0) == '\n') ++*row;
}

