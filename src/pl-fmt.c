/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        jan@swi.psy.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2004, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Formatted output (Prolog predicates format/[1,2,3]).   One  day,  the  C
source should also use format() to produce error messages, etc.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "pl-incl.h"
#include "pl-ctype.h"
#include "pl-utf8.h"
#include <ctype.h>

static char *	formatNumber(bool split, int div, int radix,
			     bool small, Number n, Buffer out);
static char *	formatFloat(int how, int arg, Number f, Buffer out);

#define MAXRUBBER 100

struct rubber
{ int where;				/* where is rubber in output */
  int size;				/* how big should it be */
  pl_wchar_t pad;			/* padding character */
};

typedef struct
{ IOSTREAM *out;			/* our output stream */
  int column;				/* current column */
  tmp_buffer buffer;			/* bin for characters with tabs */
  int buffered;				/* characters in buffer */
  int pending_rubber;			/* number of not-filled ~t's */
  struct rubber rub[MAXRUBBER];
} format_state;

#define BUFSIZE 	1024
#define DEFAULT 	(-1)
#define SHIFT   	{ argc--; argv++; }
#define NEED_ARG	{ if ( argc <= 0 ) \
			  { FMT_ERROR("not enough arguments"); \
			  } \
			}
#define FMT_ERROR(fmt)	return (void)Sunlock(fd), \
			  PL_error(NULL, 0, NULL, ERR_FORMAT, fmt)
#define FMT_ARG(c, a)	return (void)Sunlock(fd), \
			       PL_error(NULL, 0, NULL, \
					ERR_FORMAT_ARG, c, a)


static int
update_column(int col, int c)
{ switch(c)
  { case '\n':	return 0;
    case '\r':  return 0;
    case '\t':	return (col + 1) | 0x7;
    case '\b':	return (col <= 0 ? 0 : col - 1);
    default:	return col + 1;
  }
}   


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Low-level output. If there is pending  rubber   the  output is stored in
UTF-8 format in the state's `buffer'.   The  `buffered' field represents
the number of UTF-8 characters in the buffer.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
outchr(format_state *state, int chr)
{ if ( state->pending_rubber )
  { if ( chr > 0x7f )
    { char buf[8];
      char *s, *e;

      e = utf8_put_char(buf, chr);
      for(s=buf; s<e; s++)
	addBuffer((Buffer)&state->buffer, *s, char);
    } else
    { char c = chr;

      addBuffer((Buffer)&state->buffer, c, char);
    }

    state->buffered++;
  } else
  { if ( Sputcode(chr, state->out) < 0 )
      return FALSE;
  }

  state->column = update_column(state->column, chr);

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Emit ASCII 0-terminated strings resulting from sprintf() on numeric
arguments.  No fuzz with wide characters here.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
outstring(format_state *state, const char *s, unsigned int len)
{ const char *q;
  const char *e = &s[len];

  if ( state->pending_rubber )
  { addMultipleBuffer(&state->buffer, s, len, char);
    state->buffered += len;
  } else
  { for(q=s; q < e; q++)
    { if ( Sputcode(*q&0xff, state->out) < 0 )
	return FALSE;
    }
  }

  for(q=s; q < e; q++)
    state->column = update_column(state->column, *q&0xff);

  return TRUE;
}


static int
outstring0(format_state *state, const char *s)
{ return outstring(state, s, strlen(s));
}


static int
oututf8(format_state *state, const char *s, unsigned int len)
{ const char *e = &s[len];

  while(s<e)
  { int chr;

    s = utf8_get_char(s, &chr);
    if ( !outchr(state, chr) )
      return FALSE;
  }

  return TRUE;
}


static int
outtext(format_state *state, PL_chars_t *txt)
{ switch(txt->encoding)
  { case ENC_ISO_LATIN_1:
      return outstring(state, txt->text.t, txt->length);
    case ENC_WCHAR:
    { const pl_wchar_t *s = txt->text.w;
      const pl_wchar_t *e = &s[txt->length];

      while(s<e)
      { if ( !outchr(state, *s++) )
	  return FALSE;
      }

      return TRUE;
    }
    default:
    { assert(0);
      return FALSE;
    }
  }
}


#define format_predicates (GD->format.predicates)

static int	update_column(int, Char);
static bool	do_format(IOSTREAM *fd, PL_chars_t *fmt, int ac, term_t av);
static void	distribute_rubber(struct rubber *, int, int);
static int	emit_rubber(format_state *state);


		/********************************
		*       PROLOG CONNECTION	*
		********************************/

word
pl_format_predicate(term_t chr, term_t descr)
{ int c;
  Procedure proc;
  Symbol s;

  if ( !PL_get_char_ex(chr, &c, FALSE) )
    fail;

  if ( !get_procedure(descr, &proc, 0, GP_CREATE) )
    fail;
  if ( proc->definition->functor->arity == 0 )
    return PL_error(NULL, 0, "arity must be > 0", ERR_DOMAIN,
		    PL_new_atom("format_predicate"),
		    descr);

  if ( !format_predicates )
    format_predicates = newHTable(8);
  
  if ( (s = lookupHTable(format_predicates, (void *)(long)c)) )
    s->value = proc;
  else
    addHTable(format_predicates, (void *)(long)c, proc);

  succeed;
}


word
pl_current_format_predicate(term_t chr, term_t descr, control_t h)
{ Symbol s = NULL;
  TableEnum e;
  mark m;

  switch( ForeignControl(h) )
  { case FRG_FIRST_CALL:
      if ( !format_predicates )
	fail;
      e = newTableEnum(format_predicates);
      break;
    case FRG_REDO:
      e = ForeignContextPtr(h);
      break;
    case FRG_CUTTED:
      e = ForeignContextPtr(h);
      freeTableEnum(e);
    default:
      succeed;
  }

  while( (s=advanceTableEnum(e)) )
  { Mark(m);

    if ( PL_unify_integer(chr, (long)s->name) &&
	 unify_definition(descr, ((Procedure)s->value)->definition, 0, 0) )
    { ForeignRedoPtr(e);
    }
    Undo(m);
  }

  freeTableEnum(e);
  fail;
}


static word
format_impl(term_t stream, term_t format, term_t Args)
{ term_t argv;
  int argc = 0;
  term_t args = PL_copy_term_ref(Args);
  IOSTREAM *out;
  int rval;
  PL_chars_t fmt;

  if ( !getOutputStream(stream, &out) )
    fail;

  if ( !PL_get_text(format, &fmt, CVT_ALL|BUF_RING) )
    return PL_error("format", 3, NULL, ERR_TYPE, ATOM_text, format);

  if ( (argc = lengthList(args, FALSE)) >= 0 )
  { term_t head = PL_new_term_ref();
    int n = 0;

    argv = PL_new_term_refs(argc);
    while( PL_get_list(args, head, args) )
      PL_put_term(argv+n++, head);
  } else
  { argc = 1;
    argv = PL_new_term_refs(argc);

    PL_put_term(argv, args);
  }
  
  switch(fmt.storage)			/* format can to call-back! */
  { case PL_CHARS_RING:
    case PL_CHARS_STACK:
      PL_save_text(&fmt, BUF_MALLOC);
      break;
    default:
      break;
  }

  rval = do_format(out, &fmt, argc, argv);
  PL_free_text(&fmt);

  if ( rval )
  { return streamStatus(out);
  } else
  { PL_release_stream(out);
    fail;
  }
}


word
pl_format3(term_t stream, term_t format, term_t args)
{ int rc;

  startCritical;
  rc = format_impl(stream, format, args);
  endCritical;

  return rc;
}


word
pl_format(term_t fmt, term_t args)
{ return pl_format3(0, fmt, args);
}


static inline int
get_chr_from_text(const PL_chars_t *t, int index)
{ switch(t->encoding)
  { case ENC_ISO_LATIN_1:
      return t->text.t[index]&0xff;
    case ENC_WCHAR:
      return t->text.w[index];
    default:
      assert(0);
      return 0;				/* not reached */
  }
}


		/********************************
		*       ACTUAL FORMATTING	*
		********************************/

static bool
do_format(IOSTREAM *fd, PL_chars_t *fmt, int argc, term_t argv)
{ format_state state;			/* complete state */
  int tab_stop = 0;			/* padded tab stop */
  Symbol s;
  unsigned int here = 0;
  int rc = TRUE;

  Slock(fd);				/* buffer locally */

  state.out = fd;
  state.pending_rubber = 0;
  initBuffer(&state.buffer);
  state.buffered = 0;

  if ( fd->position )
    state.column = fd->position->linepos;
  else
    state.column = 0;

  while(here < fmt->length)
  { int c = get_chr_from_text(fmt, here);

    switch(c)
    { case '~':
	{ int arg = DEFAULT;		/* Numeric argument */
					/* Get the numeric argument */
	  c = get_chr_from_text(fmt, ++here);

	  if ( isDigitW(c) )
	  { arg = c - '0';

	    here++;
	    while(here < fmt->length)
	    { c = get_chr_from_text(fmt, here);

	      if ( isDigitW(c) )
	      { arg = arg*10 + c - '0';
		here++;
	      } else
		break;
	    }
	  } else if ( c == '*' )
	  { NEED_ARG;
	    if ( PL_get_integer(argv, &arg) )
	    { SHIFT;
	    } else
	      FMT_ERROR("no or negative integer for `*' argument");
	    c = get_chr_from_text(fmt, ++here);
	  } else if ( c == '`' && here < fmt->length )
	  { arg = get_chr_from_text(fmt, ++here);
	    c = get_chr_from_text(fmt, ++here);
	  }

					/* Check for user defined format */
	  if ( format_predicates &&
	       (s = lookupHTable(format_predicates, (Void)((long)c))) )
	  { Procedure proc = (Procedure) s->value;
	    FunctorDef fdef = proc->definition->functor;
	    term_t av = PL_new_term_refs(fdef->arity);
	    char buf[BUFSIZE];
	    char *str = buf;
	    int bufsize = BUFSIZE;
	    unsigned int i;
	    qid_t qid;

	    if ( arg == DEFAULT )
	      PL_put_atom(av+0, ATOM_default);
	    else
	      PL_put_integer(av+0, arg);

	    for(i=1; i<fdef->arity; i++)
	    { NEED_ARG;
	      PL_put_term(av+i, argv);
	      SHIFT;
	    }

	    tellString(&str, &bufsize, ENC_UTF8);
	    qid = PL_open_query(proc->definition->module, PL_Q_NODEBUG,
				proc, av);
	    PL_next_solution(qid);
	    PL_close_query(qid);
	    toldString();
	    oututf8(&state, str, bufsize);
	    if ( str != buf )
	      free(str);

	    here++;
	  } else
	  { switch(c)			/* Build in formatting */
	    { case 'a':			/* atomic */
		{ PL_chars_t txt;

		  NEED_ARG;
		  if ( !PL_get_text(argv, &txt, CVT_ATOMIC) )
		    FMT_ARG("a", argv);
		  SHIFT;
		  outtext(&state, &txt);
		  here++;
		  break;
		}
	      case 'c':			/* ~c: character code */
		{ int chr;

		  NEED_ARG;
		  if ( PL_get_integer(argv, &chr) && chr >= 0 )
		  { int times = (arg == DEFAULT ? 1 : arg);

		    SHIFT;
		    while(times-- > 0)
		    { outchr(&state, chr);
		    }
		  } else
		    FMT_ARG("c", argv);
		  here++;
		  break;
		}
	      case 'e':			/* exponential float */
	      case 'E':			/* Exponential float */
	      case 'f':			/* float */
	      case 'g':			/* shortest of 'f' and 'e' */
	      case 'G':			/* shortest of 'f' and 'E' */
		{ number n;
		  tmp_buffer b;

		  NEED_ARG;
		  if ( !valueExpression(argv, &n PASS_LD) )
		  { char f[2];
		    
		    f[0] = c;
		    f[1] = EOS;
		    FMT_ARG(f, argv);
		  }
		  SHIFT;

		  initBuffer(&b);
		  formatFloat(c, arg, &n, (Buffer)&b);
		  clearNumber(&n);
		  outstring0(&state, baseBuffer(&b, char));
		  discardBuffer(&b);
		  here++;
		  break;
		}
	      case 'd':			/* integer */
	      case 'D':			/* grouped integer */
	      case 'r':			/* radix number */
	      case 'R':			/* Radix number */
		{ number i;
		  tmp_buffer b;

		  NEED_ARG;
		  if ( !valueExpression(argv, &i PASS_LD) ||
		       !toIntegerNumber(&i) )
		  { char f[2];
		    
		    f[0] = c;
		    f[1] = EOS;
		    FMT_ARG(f, argv);
		  }
		  SHIFT;
		  if ( arg == DEFAULT )
		    arg = 0;
		  initBuffer(&b);
		  if ( c == 'd' || c == 'D' )
		  { formatNumber(c == 'D', arg, 10, TRUE, &i, (Buffer)&b);
		  } else
		  { if ( arg < 1 || arg > 36 )
		    { term_t r = PL_new_term_ref();
		      
		      PL_put_integer(r, arg);
		      Sunlock(fd);
		      return PL_error(NULL, 0, NULL, ERR_DOMAIN,
				      ATOM_radix, r);
		    }
		    formatNumber(FALSE, 0, arg, c == 'r', &i, (Buffer)&b);
		  }
		  clearNumber(&i);
		  outstring0(&state, baseBuffer(&b, char));			
		  discardBuffer(&b);
		  here++;
		  break;
		}
	      case 's':			/* string */
		{ PL_chars_t txt;

		  NEED_ARG;
		  if ( !PL_get_text(argv, &txt, CVT_LIST|CVT_STRING) )
		    FMT_ARG("s", argv);
		  outtext(&state, &txt);
		  SHIFT;
		  here++;
		  break;
		}
	      case 'i':			/* ignore */
		{ NEED_ARG;
		  SHIFT;
		  here++;
		  break;
		}
		{ Func f;
		  char buf[BUFSIZE];
		  char *str;

	      case 'k':			/* write_canonical */
		  f = pl_write_canonical; 
	          goto pl_common;
	      case 'p':			/* print */
		  f = pl_print;
	          goto pl_common;
	      case 'q':			/* writeq */
		  f = pl_writeq;
	          goto pl_common;
	      case 'w':			/* write */
		  f = pl_write;
		  pl_common:

		  NEED_ARG;
		  if ( state.pending_rubber )
		  { int bufsize = BUFSIZE;

		    str = buf;
		    tellString(&str, &bufsize, ENC_UTF8);
		    (*f)(argv);
		    toldString();
		    oututf8(&state, str, bufsize);
		    if ( str != buf )
		      free(str);
		  } else
		  { if ( fd->position &&
			 fd->position->linepos == state.column )
		    { IOSTREAM *old = Scurout;

		      Scurout = fd;
		      rc = (*f)(argv);
		      Scurout = old;
		      if ( !rc )
			goto out;

		      state.column = fd->position->linepos;
		    } else
		    { int bufsize = BUFSIZE;

		      str = buf;
		      tellString(&str, &bufsize, ENC_UTF8);
		      (*f)(argv);
		      toldString();
		      oututf8(&state, str, bufsize);
		      if ( str != buf )
			free(str);
		    }
		  }
		  SHIFT;
		  here++;
		  break;
		}
	      case 'W':			/* write_term(Value, Options) */
	       { char buf[BUFSIZE];
		 char *str;

		 if ( argc < 2 )
		 { FMT_ERROR("not enough arguments");
		 }
		 if ( state.pending_rubber )
		  { int bufsize = BUFSIZE;

		    str = buf;
		    tellString(&str, &bufsize, ENC_UTF8);
		    rc = pl_write_term(argv, argv+1);
		    toldString();
		    if ( !rc )
		      goto out;
		    oututf8(&state, str, bufsize);
		    if ( str != buf )
		      free(str);
		  } else
		  { if ( fd->position &&
			 fd->position->linepos == state.column )
		    { IOSTREAM *old = Scurout;

		      Scurout = fd;
		      rc = pl_write_term(argv, argv+1);
		      Scurout = old;
		      if ( !rc )
			goto out;

		      state.column = fd->position->linepos;
		    } else
		    { int bufsize = BUFSIZE;

		      str = buf;
		      tellString(&str, &bufsize, ENC_UTF8);
		      rc = pl_write_term(argv, argv+1);
		      if ( !rc )
			goto out;
		      toldString();
		      oututf8(&state, str, bufsize);
		      if ( str != buf )
			free(str);
		    }
		  }
		  SHIFT;
		  SHIFT;
		  here++;
		  break;
	       }
	      case '@':
	        { char buf[BUFSIZE];
		  char *str = buf;
		  int bufsize = BUFSIZE;
		  term_t ex = 0;
		  int rval;

		  if ( argc < 1 )
		  { FMT_ERROR("not enough arguments");
		  }
		  tellString(&str, &bufsize, ENC_UTF8);
		  rval = callProlog(NULL, argv, PL_Q_CATCH_EXCEPTION, &ex);
		  toldString();
		  oututf8(&state, str, bufsize);
		  if ( str != buf )
		    free(str);

		  if ( !rval && ex )
		  { Sunlock(fd);
		    return PL_raise_exception(ex);
		  }

		  SHIFT;
		  here++;
		  break;
	        }
	      case '~':			/* ~ */
		{ outchr(&state, '~');
		  here++;
		  break;
		}
	      case 'n':			/* \n */
	      case 'N':			/* \n if not on newline */
		{ if ( arg == DEFAULT )
		    arg = 1;
		  if ( c == 'N' && state.column == 0 )
		    arg--;
		  while( arg-- > 0 )
		    outchr(&state, '\n');
		  here++;
		  break;
		}
	      case 't':			/* insert tab */
		{ if ( state.pending_rubber >= MAXRUBBER )
		    FMT_ERROR("Too many tab stops");

		  state.rub[state.pending_rubber].where = state.buffered;
		  state.rub[state.pending_rubber].pad   =
					(arg == DEFAULT ? (pl_wchar_t)' '
							: (pl_wchar_t)arg);
		  state.rub[state.pending_rubber].size = 0;
		  state.pending_rubber++;
		  here++;
		  break;
		}
	      case '|':			/* set tab */
		{ int stop;

		  if ( arg == DEFAULT )
		    arg = state.column;
	      case '+':			/* tab relative */
		  if ( arg == DEFAULT )
		    arg = 8;
		  stop = (c == '+' ? tab_stop + arg : arg);

		  if ( state.pending_rubber == 0 ) /* nothing to distribute */
		  { state.rub[0].where = state.buffered;
		    state.rub[0].pad = ' ';
		    state.pending_rubber++;
		  }
		  distribute_rubber(state.rub,
				    state.pending_rubber,
				    stop - state.column);
		  emit_rubber(&state);

		  state.column = tab_stop = stop;
		  here++;
		  break;
		}
	      default:
	      { term_t ex = PL_new_term_ref();

		Sunlock(fd);
		PL_put_atom(ex, codeToAtom(*(unsigned char *)fmt));
		return PL_error("format", 2, NULL, ERR_EXISTENCE,
				PL_new_atom("format_character"),
				ex);
	      }
	    }
	  }
	  break;			/* the '~' switch */
	}
      default:
	{ outchr(&state, c);
	  here++;
	  break;
	}
    }
  }

  if ( state.pending_rubber )		/* not closed ~t: flush out */
    emit_rubber(&state);

out:
  Sunlock(fd);

  return rc;
}


static void
distribute_rubber(struct rubber *r, int rn, int space)
{ if ( space > 0 )
  { int s = space / rn;
    int n, m;

    for(n=0; n < rn; n++)		/* give them equal size */
      r[n].size = s;
					/* distribute from the center */
    space -= s*rn;
    for(m = rn / 2, n = 0; space; n++, space--)
    { r[m + (n % 2 ? n : -n)].size++;
    }
  } else
  { int n;

    for(n=0; n < rn; n++)		/* set all rubber to 0 */
      r[n].size = 0;
  }
}


static int
emit_rubber(format_state *state)
{ const char *s = baseBuffer(&state->buffer, char);
  const char *e = &s[entriesBuffer(&state->buffer, char)];
  struct rubber *r = state->rub;
  int rn = state->pending_rubber;
  int j;

  for(j = 0; s <= e; j++)
  { int chr;

    if ( r->where == j && rn )
    { int n;

      for(n=0; n<r->size; n++)
      { if ( Sputcode(r->pad, state->out) < 0 )
	  return FALSE;
      }
      r++;
      rn--;
    }

    if ( s < e )
    { s = utf8_get_char(s, &chr);
      if ( Sputcode(chr, state->out) < 0 )
	return FALSE;
    } else
      break;
  }

  discardBuffer(&state->buffer);
  initBuffer(&state->buffer);
  state->buffered = 0;
  state->pending_rubber = 0;

  return TRUE;
}


/*  format an integer according to  a  number  of  modifiers  at various
    radius.   `split'  is a boolean asking to put ',' between each group
    of three digits (e.g. 67,567,288).  `div' askes to divide the number
    by radix^`div' before printing.   `radix'  is  the  radix  used  for
    conversion.  `n' is the number to be converted.

 ** Fri Aug 19 22:26:41 1988  jan@swivax.UUCP (Jan Wielemaker)  */

static char *
formatNumber(bool split, int div, int radix, bool small, Number i,
	     Buffer out)
{ switch(i->type)
  { case V_INTEGER:
    { int64_t n = i->value.i;

      char tmp[100]; 
      char *end = &tmp[sizeof(tmp)];
      char *s = end;			/* i.e. start at the end */
      int before = (div == 0);
      int digits = 0;
      bool negative = FALSE;

      *--s = EOS;
      if ( n < 0 )
      { n = -n;
	negative = TRUE;
      }
      if ( n == 0 && div == 0 )
      { *--s = '0';
      } else
      { while( n > 0 || div >= 0 )
	{ if ( div-- == 0 && !before )
	  { *--s = '.';
	    before = 1;
	  }
	  if ( split && before && (digits++ % 3) == 0 && digits != 1 )
	    *--s = ',';
	  *--s = digitName((int)(n % radix), small);
	  n /= radix;
	}
	if ( negative )
	  *--s = '-';  
      }

      addMultipleBuffer(out, s, end-s, char);

      return baseBuffer(out, char);
    }
#ifdef O_GMP
    case V_MPZ:
    { int len = mpz_sizeinbase(i->value.mpz, radix);
      char tmp[256];
      char *buf;

      if ( len+2 > sizeof(tmp) )
	buf = PL_malloc(len+2);
      else
	buf = tmp;

      mpz_get_str(buf, radix, i->value.mpz);
      if ( !small && radix > 10 )
      { char *s;

	for(s=buf; *s; s++)
	  *s = toupper(*s);
      }
      if ( split || div > 0 )
      { int before = len-div;
	int leading;
	char *s = buf;

	if ( *s == '-' )
	{ addBuffer(out, *s, char);
	  s++;
	}
	if ( split )
	{ leading = before % 3;
	  if ( leading == 0 )
	    leading = 3;
	} else
	{ leading = len;
	}
	for(s=buf; *s; s++)
	{ if ( before-- == 0 && div > 0 )
	  { addBuffer(out, '.', char);
	  } else if ( leading-- == 0 && before > 0 )
	  { addBuffer(out, ',', char);
	    leading = 2;
	  }
	  addBuffer(out, *s, char);
	}
	addBuffer(out, EOS, char);
      } else
      { addMultipleBuffer(out, buf, strlen(buf), char);
	addBuffer(out, EOS, char);
      }

      if ( buf != tmp )
	PL_free(buf);

      return baseBuffer(out, char);
    }
#endif /*O_GMP*/
    default:
      assert(0);
      return NULL;
  }
}


static char *
formatFloat(int how, int arg, Number f, Buffer out)
{ if ( arg == DEFAULT )
    arg = 6;

  switch(f->type)
  { 
#ifdef O_GMP
    mpf_t mpf;
    case V_MPZ:
      mpf_init2(mpf, arg*4);
      mpf_set_z(mpf, f->value.mpz);
      goto print;
    case V_MPQ:
    { char tmp[12];
      int size;
      int written;
      int fbits;
      
      switch(how)
      { case 'f':
	case 'g':
	case 'G':
	{ mpz_t iv;

	  mpz_init(iv);
	  mpz_set_q(iv, f->value.mpq);
	  fbits = mpz_sizeinbase(iv, 2) + 4*arg;
	  mpz_clear(iv);
	  break;
	}
	default:
	  fbits = 4*arg;
      }
      
      mpf_init2(mpf, fbits);
      mpf_set_q(mpf, f->value.mpq);
      
    print:
      Ssprintf(tmp, "%%.%dF%c", arg, how);
      size = 0;
      written = arg+4;
      while(written >= size)
      { size = written+1;

	growBuffer(out, size);		/* reserve for -.e<null> */
	written = gmp_snprintf(baseBuffer(out, char), size, tmp, mpf);
      }
      mpf_clear(mpf);
      out->top = out->base + written;

      return baseBuffer(out, char);
    }
#endif
    case V_INTEGER:
      promoteToRealNumber(f);
      /*FALLTHROUGH*/
    case V_REAL:
    { char tmp[12];

      if ( arg > 256 )
	arg = 256;

      growBuffer(out, 256+4);		/* reserve for -.e<null> */
      Ssprintf(tmp, "%%.%d%c", arg, how);
      Ssprintf(baseBuffer(out, char), tmp, f->value.f);
      out->top = out->base + strlen(out->base);

      return baseBuffer(out, char);
    }
  }

  return NULL;
}
