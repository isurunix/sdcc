/*-------------------------------------------------------------------------
  SDCCgen51.c - source file for code generation for 8051

  Written By -  Sandeep Dutta . sandeep.dutta@usa.net (1998)
         and -  Jean-Louis VERN.jlvern@writeme.com (1999)
  Bug Fixes  -  Wojciech Stryjewski  wstryj1@tiger.lsu.edu (1999 v2.1.9a)
  PIC port   -  Scott Dattalo scott@dattalo.com (2000)

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

  In other words, you are welcome to use, share and improve this program.
  You are forbidden to forbid anyone else to use, share and improve
  what you give them.   Help stamp out software-hoarding!

  Notes:
  000123 mlh  Moved aopLiteral to SDCCglue.c to help the split
      Made everything static
-------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "SDCCglobl.h"
#include "newalloc.h"

#if defined(_MSC_VER)
#define __FUNCTION__    __FILE__
#endif

#ifdef HAVE_SYS_ISA_DEFS_H
#include <sys/isa_defs.h>
#else
#ifdef HAVE_ENDIAN_H
#include <endian.h>
#else
#if !defined(__BORLANDC__) && !defined(_MSC_VER)
#warning "Cannot determine ENDIANESS of this machine assuming LITTLE_ENDIAN"
#warning "If you running sdcc on an INTEL 80x86 Platform you are okay"
#endif
#endif
#endif

#include "common.h"
#include "SDCCpeeph.h"
#include "ralloc.h"
#include "gen.h"

//char *aopLiteral (value *val, int offset);
char *pic14aopLiteral (value *val, int offset);

/* this is the down and dirty file with all kinds of
   kludgy & hacky stuff. This is what it is all about
   CODE GENERATION for a specific MCU . some of the
   routines may be reusable, will have to see */

static char *zero = "#0x00";
static char *one  = "#0x01";
static char *spname = "sp";

char *fReturnpic14[] = {"fsr","dph","b","a" };
//char *fReturn390[] = {"dpl","dph","dpx", "b","a" };
static unsigned fReturnSize = 4; /* shared with ralloc.c */
static char **fReturn = fReturnpic14;

static char *accUse[] = {"a","b"};

//static short rbank = -1;

static struct {
    short r0Pushed;
    short r1Pushed;
    short accInUse;
    short inLine;
    short debugLine;
    short nRegsSaved;
    set *sendSet;
} _G;

extern int pic14_ptrRegReq ;
extern int pic14_nRegs;
extern FILE *codeOutFile;
static void saverbank (int, iCode *,bool);
#define RESULTONSTACK(x) \
                         (IC_RESULT(x) && IC_RESULT(x)->aop && \
                         IC_RESULT(x)->aop->type == AOP_STK )

#define MOVA(x) if (strcmp(x,"a") && strcmp(x,"acc")) emitcode(";XXX mov","a,%s  %s,%d",x,__FILE__,__LINE__);
#define CLRC    emitcode(";XXX clr","c %s,%d",__FILE__,__LINE__);

#define BIT_NUMBER(x) (x & 7)
#define BIT_REGISTER(x) (x>>3)

static lineNode *lineHead = NULL;
static lineNode *lineCurr = NULL;

static unsigned char   SLMask[] = {0xFF ,0xFE, 0xFC, 0xF8, 0xF0,
0xE0, 0xC0, 0x80, 0x00};
static unsigned char   SRMask[] = {0xFF, 0x7F, 0x3F, 0x1F, 0x0F,
0x07, 0x03, 0x01, 0x00};

#define LSB     0
#define MSB16   1
#define MSB24   2
#define MSB32   3


#define FUNCTION_LABEL_INC  20
static int labelOffset=0;
static int debug_verbose=0;


/*-----------------------------------------------------------------*/
/* Macros for emitting instructions                                */
/*-----------------------------------------------------------------*/
#define emitSKPC    emitcode("btfss","status,c")
#define emitSKPNC   emitcode("btfsc","status,c")
#define emitSKPZ    emitcode("btfss","status,z")
#define emitSKPNZ   emitcode("btfsc","status,z")

/*-----------------------------------------------------------------*/
/*  my_powof2(n) - If `n' is an integaer power of 2, then the      */
/*                 exponent of 2 is returned, otherwise -1 is      */
/*                 returned.                                       */
/* note that this is similar to the function `powof2' in SDCCsymt  */
/* if(n == 2^y)                                                    */
/*   return y;                                                     */
/* return -1;                                                      */
/*-----------------------------------------------------------------*/
static int my_powof2 (unsigned long num)
{
  if(num) {
    if( (num & (num-1)) == 0) {
      int nshifts = -1;
      while(num) {
  num>>=1;
  nshifts++;
      }
      return nshifts;
    }
  }

  return -1;
}


/*-----------------------------------------------------------------*/
/* emitcode - writes the code into a file : for now it is simple    */
/*-----------------------------------------------------------------*/
static void emitcode (char *inst,char *fmt, ...)
{
    va_list ap;
    char lb[MAX_INLINEASM];
    char *lbp = lb;

    va_start(ap,fmt);

    if (inst && *inst) {
  if (fmt && *fmt)
      sprintf(lb,"%s\t",inst);
  else
      sprintf(lb,"%s",inst);
        vsprintf(lb+(strlen(lb)),fmt,ap);
    }  else
        vsprintf(lb,fmt,ap);

    while (isspace(*lbp)) lbp++;

    if (lbp && *lbp)
        lineCurr = (lineCurr ?
                    connectLine(lineCurr,newLineNode(lb)) :
                    (lineHead = newLineNode(lb)));
    lineCurr->isInline = _G.inLine;
    lineCurr->isDebug  = _G.debugLine;
    va_end(ap);
}

static void DEBUGemitcode (char *inst,char *fmt, ...)
{
    va_list ap;
    char lb[MAX_INLINEASM];
    char *lbp = lb;

    if(!debug_verbose)
      return;

    va_start(ap,fmt);

    if (inst && *inst) {
  if (fmt && *fmt)
      sprintf(lb,"%s\t",inst);
  else
      sprintf(lb,"%s",inst);
        vsprintf(lb+(strlen(lb)),fmt,ap);
    }  else
        vsprintf(lb,fmt,ap);

    while (isspace(*lbp)) lbp++;

    if (lbp && *lbp)
        lineCurr = (lineCurr ?
                    connectLine(lineCurr,newLineNode(lb)) :
                    (lineHead = newLineNode(lb)));
    lineCurr->isInline = _G.inLine;
    lineCurr->isDebug  = _G.debugLine;
    va_end(ap);
}


/*-----------------------------------------------------------------*/
/* getFreePtr - returns r0 or r1 whichever is free or can be pushed*/
/*-----------------------------------------------------------------*/
static regs *getFreePtr (iCode *ic, asmop **aopp, bool result)
{
    bool r0iu = FALSE , r1iu = FALSE;
    bool r0ou = FALSE , r1ou = FALSE;

    /* the logic: if r0 & r1 used in the instruction
    then we are in trouble otherwise */

    /* first check if r0 & r1 are used by this
    instruction, in which case we are in trouble */
    if ((r0iu = bitVectBitValue(ic->rUsed,R0_IDX)) &&
        (r1iu = bitVectBitValue(ic->rUsed,R1_IDX)))
    {
        goto endOfWorld;
    }

    r0ou = bitVectBitValue(ic->rMask,R0_IDX);
    r1ou = bitVectBitValue(ic->rMask,R1_IDX);

    /* if no usage of r0 then return it */
    if (!r0iu && !r0ou) {
        ic->rUsed = bitVectSetBit(ic->rUsed,R0_IDX);
        (*aopp)->type = AOP_R0;

        return (*aopp)->aopu.aop_ptr = pic14_regWithIdx(R0_IDX);
    }

    /* if no usage of r1 then return it */
    if (!r1iu && !r1ou) {
        ic->rUsed = bitVectSetBit(ic->rUsed,R1_IDX);
        (*aopp)->type = AOP_R1;

        return (*aopp)->aopu.aop_ptr = pic14_regWithIdx(R1_IDX);
    }

    /* now we know they both have usage */
    /* if r0 not used in this instruction */
    if (!r0iu) {
        /* push it if not already pushed */
        if (!_G.r0Pushed) {
            emitcode ("push","%s",
                      pic14_regWithIdx(R0_IDX)->dname);
            _G.r0Pushed++ ;
        }

        ic->rUsed = bitVectSetBit(ic->rUsed,R0_IDX);
        (*aopp)->type = AOP_R0;

        return (*aopp)->aopu.aop_ptr = pic14_regWithIdx(R0_IDX);
    }

    /* if r1 not used then */

    if (!r1iu) {
        /* push it if not already pushed */
        if (!_G.r1Pushed) {
            emitcode ("push","%s",
                      pic14_regWithIdx(R1_IDX)->dname);
            _G.r1Pushed++ ;
        }

        ic->rUsed = bitVectSetBit(ic->rUsed,R1_IDX);
        (*aopp)->type = AOP_R1;
        return pic14_regWithIdx(R1_IDX);
    }

endOfWorld :
    /* I said end of world but not quite end of world yet */
    /* if this is a result then we can push it on the stack*/
    if (result) {
        (*aopp)->type = AOP_STK;
        return NULL;
    }

    piCode(ic,stdout);
    /* other wise this is true end of the world */
    werror(E_INTERNAL_ERROR,__FILE__,__LINE__,
           "getFreePtr should never reach here");
    exit(0);
}

/*-----------------------------------------------------------------*/
/* newAsmop - creates a new asmOp                                  */
/*-----------------------------------------------------------------*/
static asmop *newAsmop (short type)
{
    asmop *aop;

    aop = Safe_calloc(1,sizeof(asmop));
    aop->type = type;
    return aop;
}

static void genSetDPTR(int n)
{
    if (!n)
    {
        emitcode(";", "Select standard DPTR");
        emitcode("mov", "dps, #0x00");
    }
    else
    {
        emitcode(";", "Select alternate DPTR");
        emitcode("mov", "dps, #0x01");
    }
}

/*-----------------------------------------------------------------*/
/* pointerCode - returns the code for a pointer type               */
/*-----------------------------------------------------------------*/
static int pointerCode (sym_link *etype)
{

    return PTR_TYPE(SPEC_OCLS(etype));

}

/*-----------------------------------------------------------------*/
/* aopForSym - for a true symbol                                   */
/*-----------------------------------------------------------------*/
static asmop *aopForSym (iCode *ic,symbol *sym,bool result)
{
    asmop *aop;
    memmap *space= SPEC_OCLS(sym->etype);

    DEBUGemitcode("; ***","%s %d",__FUNCTION__,__LINE__);
    /* if already has one */
    if (sym->aop)
        return sym->aop;

    /* assign depending on the storage class */
    /* if it is on the stack or indirectly addressable */
    /* space we need to assign either r0 or r1 to it   */
    if ((sym->onStack && !options.stack10bit) || sym->iaccess) {
        sym->aop = aop = newAsmop(0);
        aop->aopu.aop_ptr = getFreePtr(ic,&aop,result);
        aop->size = getSize(sym->type);

        /* now assign the address of the variable to
        the pointer register */
        if (aop->type != AOP_STK) {

            if (sym->onStack) {
                    if ( _G.accInUse )
                        emitcode("push","acc");

                    emitcode("mov","a,_bp");
                    emitcode("add","a,#0x%02x",
                             ((sym->stack < 0) ?
            ((char)(sym->stack - _G.nRegsSaved )) :
            ((char)sym->stack)) & 0xff);
                    emitcode("mov","%s,a",
                             aop->aopu.aop_ptr->name);

                    if ( _G.accInUse )
                        emitcode("pop","acc");
            } else
                emitcode("mov","%s,#%s",
                         aop->aopu.aop_ptr->name,
                         sym->rname);
            aop->paged = space->paged;
        } else
            aop->aopu.aop_stk = sym->stack;
        return aop;
    }

    if (sym->onStack && options.stack10bit)
    {
        /* It's on the 10 bit stack, which is located in
         * far data space.
         */

      //DEBUGemitcode(";","%d",__LINE__);

        if ( _G.accInUse )
          emitcode("push","acc");

        emitcode("mov","a,_bp");
        emitcode("add","a,#0x%02x",
                 ((sym->stack < 0) ?
                   ((char)(sym->stack - _G.nRegsSaved )) :
                   ((char)sym->stack)) & 0xff);

        genSetDPTR(1);
        emitcode ("mov","dpx1,#0x40");
        emitcode ("mov","dph1,#0x00");
        emitcode ("mov","dpl1, a");
        genSetDPTR(0);

        if ( _G.accInUse )
            emitcode("pop","acc");

        sym->aop = aop = newAsmop(AOP_DPTR2);
      aop->size = getSize(sym->type);
      return aop;
    }

    //DEBUGemitcode(";","%d",__LINE__);
    /* if in bit space */
    if (IN_BITSPACE(space)) {
        sym->aop = aop = newAsmop (AOP_CRY);
        aop->aopu.aop_dir = sym->rname ;
        aop->size = getSize(sym->type);
  DEBUGemitcode(";","%d sym->rname = %s, size = %d",__LINE__,sym->rname,aop->size);
        return aop;
    }
    /* if it is in direct space */
    if (IN_DIRSPACE(space)) {
        sym->aop = aop = newAsmop (AOP_DIR);
        aop->aopu.aop_dir = sym->rname ;
        aop->size = getSize(sym->type);
  DEBUGemitcode(";","%d sym->rname = %s, size = %d",__LINE__,sym->rname,aop->size);
        return aop;
    }

    /* special case for a function */
    if (IS_FUNC(sym->type)) {
        sym->aop = aop = newAsmop(AOP_IMMD);
        aop->aopu.aop_immd = Safe_calloc(1,strlen(sym->rname)+1);
        strcpy(aop->aopu.aop_immd,sym->rname);
        aop->size = FPTRSIZE;
        return aop;
    }


    /* only remaining is far space */
    /* in which case DPTR gets the address */
    sym->aop = aop = newAsmop(AOP_DPTR);
    emitcode ("mov","dptr,#%s", sym->rname);
    aop->size = getSize(sym->type);

    DEBUGemitcode(";","%d size = %d",__LINE__,aop->size);
    /* if it is in code space */
    if (IN_CODESPACE(space))
        aop->code = 1;

    return aop;
}

/*-----------------------------------------------------------------*/
/* aopForRemat - rematerialzes an object                           */
/*-----------------------------------------------------------------*/
static asmop *aopForRemat (symbol *sym)
{
    iCode *ic = sym->rematiCode;
    asmop *aop = newAsmop(AOP_IMMD);
    int val = 0;
    DEBUGemitcode(";","%s %d",__FUNCTION__,__LINE__);
    for (;;) {
      if (ic->op == '+')
      val += operandLitValue(IC_RIGHT(ic));
  else if (ic->op == '-')
      val -= operandLitValue(IC_RIGHT(ic));
  else
      break;

  ic = OP_SYMBOL(IC_LEFT(ic))->rematiCode;
    }

    if (val)
      sprintf(buffer,"(%s %c 0x%04x)",
          OP_SYMBOL(IC_LEFT(ic))->rname,
    val >= 0 ? '+' : '-',
    abs(val) & 0xffff);
    else
  strcpy(buffer,OP_SYMBOL(IC_LEFT(ic))->rname);

    //DEBUGemitcode(";","%s",buffer);
    aop->aopu.aop_immd = Safe_calloc(1,strlen(buffer)+1);
    strcpy(aop->aopu.aop_immd,buffer);
    return aop;
}

/*-----------------------------------------------------------------*/
/* regsInCommon - two operands have some registers in common       */
/*-----------------------------------------------------------------*/
static bool regsInCommon (operand *op1, operand *op2)
{
    symbol *sym1, *sym2;
    int i;

    /* if they have registers in common */
    if (!IS_SYMOP(op1) || !IS_SYMOP(op2))
        return FALSE ;

    sym1 = OP_SYMBOL(op1);
    sym2 = OP_SYMBOL(op2);

    if (sym1->nRegs == 0 || sym2->nRegs == 0)
        return FALSE ;

    for (i = 0 ; i < sym1->nRegs ; i++) {
        int j;
        if (!sym1->regs[i])
            continue ;

        for (j = 0 ; j < sym2->nRegs ;j++ ) {
            if (!sym2->regs[j])
                continue ;

            if (sym2->regs[j] == sym1->regs[i])
                return TRUE ;
        }
    }

    return FALSE ;
}

/*-----------------------------------------------------------------*/
/* operandsEqu - equivalent                                        */
/*-----------------------------------------------------------------*/
static bool operandsEqu ( operand *op1, operand *op2)
{
    symbol *sym1, *sym2;

    /* if they not symbols */
    if (!IS_SYMOP(op1) || !IS_SYMOP(op2))
        return FALSE;

    sym1 = OP_SYMBOL(op1);
    sym2 = OP_SYMBOL(op2);

    /* if both are itemps & one is spilt
       and the other is not then false */
    if (IS_ITEMP(op1) && IS_ITEMP(op2) &&
  sym1->isspilt != sym2->isspilt )
  return FALSE ;

    /* if they are the same */
    if (sym1 == sym2)
        return TRUE ;

    if (strcmp(sym1->rname,sym2->rname) == 0)
        return TRUE;


    /* if left is a tmp & right is not */
    if (IS_ITEMP(op1)  &&
        !IS_ITEMP(op2) &&
        sym1->isspilt  &&
        (sym1->usl.spillLoc == sym2))
        return TRUE;

    if (IS_ITEMP(op2)  &&
        !IS_ITEMP(op1) &&
        sym2->isspilt  &&
  sym1->level > 0 &&
        (sym2->usl.spillLoc == sym1))
        return TRUE ;

    return FALSE ;
}

/*-----------------------------------------------------------------*/
/* sameRegs - two asmops have the same registers                   */
/*-----------------------------------------------------------------*/
static bool sameRegs (asmop *aop1, asmop *aop2 )
{
    int i;

    if (aop1 == aop2)
        return TRUE ;

    if (aop1->type != AOP_REG ||
        aop2->type != AOP_REG )
        return FALSE ;

    if (aop1->size != aop2->size )
        return FALSE ;

    for (i = 0 ; i < aop1->size ; i++ )
        if (aop1->aopu.aop_reg[i] !=
            aop2->aopu.aop_reg[i] )
            return FALSE ;

    return TRUE ;
}

/*-----------------------------------------------------------------*/
/* aopOp - allocates an asmop for an operand  :                    */
/*-----------------------------------------------------------------*/
static void aopOp (operand *op, iCode *ic, bool result)
{
    asmop *aop;
    symbol *sym;
    int i;

    if (!op)
        return ;

    DEBUGemitcode(";","%d",__LINE__);
    /* if this a literal */
    if (IS_OP_LITERAL(op)) {
      DEBUGemitcode(";","%d",__LINE__);
        op->aop = aop = newAsmop(AOP_LIT);
        aop->aopu.aop_lit = op->operand.valOperand;
        aop->size = getSize(operandType(op));
        return;
    }

    /* if already has a asmop then continue */
    if (op->aop)
        return ;

    /* if the underlying symbol has a aop */
    if (IS_SYMOP(op) && OP_SYMBOL(op)->aop) {
      DEBUGemitcode(";","%d",__LINE__);
        op->aop = OP_SYMBOL(op)->aop;
        return;
    }

    /* if this is a true symbol */
    if (IS_TRUE_SYMOP(op)) {
      DEBUGemitcode(";","%d",__LINE__);
        op->aop = aopForSym(ic,OP_SYMBOL(op),result);
        return ;
    }

    /* this is a temporary : this has
    only four choices :
    a) register
    b) spillocation
    c) rematerialize
    d) conditional
    e) can be a return use only */

    sym = OP_SYMBOL(op);


    /* if the type is a conditional */
    if (sym->regType == REG_CND) {
      DEBUGemitcode(";","%d",__LINE__);
        aop = op->aop = sym->aop = newAsmop(AOP_CRY);
        aop->size = 0;
        return;
    }

    /* if it is spilt then two situations
    a) is rematerialize
    b) has a spill location */
    if (sym->isspilt || sym->nRegs == 0) {

      DEBUGemitcode(";","%d",__LINE__);
        /* rematerialize it NOW */
        if (sym->remat) {
            sym->aop = op->aop = aop =
                                      aopForRemat (sym);
            aop->size = getSize(sym->type);
      DEBUGemitcode(";","%d",__LINE__);
            return;
        }

  if (sym->accuse) {
      int i;
            aop = op->aop = sym->aop = newAsmop(AOP_ACC);
            aop->size = getSize(sym->type);
            for ( i = 0 ; i < 2 ; i++ )
                aop->aopu.aop_str[i] = accUse[i];
      DEBUGemitcode(";","%d",__LINE__);
            return;
  }

        if (sym->ruonly ) {
            int i;
            aop = op->aop = sym->aop = newAsmop(AOP_STR);
            aop->size = getSize(sym->type);
            for ( i = 0 ; i < fReturnSize ; i++ )
        aop->aopu.aop_str[i] = fReturn[i];
      DEBUGemitcode(";","%d",__LINE__);
            return;
        }

        /* else spill location  */
  DEBUGemitcode(";","%s %d %s",__FUNCTION__,__LINE__,sym->usl.spillLoc->rname);
        sym->aop = op->aop = aop =
                                  aopForSym(ic,sym->usl.spillLoc,result);
        aop->size = getSize(sym->type);
        return;
    }

    /* must be in a register */
    sym->aop = op->aop = aop = newAsmop(AOP_REG);
    aop->size = sym->nRegs;
    for ( i = 0 ; i < sym->nRegs ;i++)
        aop->aopu.aop_reg[i] = sym->regs[i];
}

/*-----------------------------------------------------------------*/
/* freeAsmop - free up the asmop given to an operand               */
/*----------------------------------------------------------------*/
static void freeAsmop (operand *op, asmop *aaop, iCode *ic, bool pop)
{
    asmop *aop ;

    if (!op)
        aop = aaop;
    else
        aop = op->aop;

    if (!aop)
        return ;

    if (aop->freed)
        goto dealloc;

    aop->freed = 1;

    /* depending on the asmop type only three cases need work AOP_RO
       , AOP_R1 && AOP_STK */
    switch (aop->type) {
        case AOP_R0 :
            if (_G.r0Pushed ) {
                if (pop) {
                    emitcode ("pop","ar0");
                    _G.r0Pushed--;
                }
            }
            bitVectUnSetBit(ic->rUsed,R0_IDX);
            break;

        case AOP_R1 :
            if (_G.r1Pushed ) {
                if (pop) {
                    emitcode ("pop","ar1");
                    _G.r1Pushed--;
                }
            }
            bitVectUnSetBit(ic->rUsed,R1_IDX);
            break;

        case AOP_STK :
        {
            int sz = aop->size;
            int stk = aop->aopu.aop_stk + aop->size;
            bitVectUnSetBit(ic->rUsed,R0_IDX);
            bitVectUnSetBit(ic->rUsed,R1_IDX);

            getFreePtr(ic,&aop,FALSE);

            if (options.stack10bit)
            {
                /* I'm not sure what to do here yet... */
                /* #STUB */
              fprintf(stderr,
                "*** Warning: probably generating bad code for "
                "10 bit stack mode.\n");
            }

            if (stk) {
                emitcode ("mov","a,_bp");
                emitcode ("add","a,#0x%02x",((char)stk) & 0xff);
                emitcode ("mov","%s,a",aop->aopu.aop_ptr->name);
            } else {
                emitcode ("mov","%s,_bp",aop->aopu.aop_ptr->name);
            }

            while (sz--) {
                emitcode("pop","acc");
                emitcode("mov","@%s,a",aop->aopu.aop_ptr->name);
                if (!sz) break;
                emitcode("dec","%s",aop->aopu.aop_ptr->name);
            }
            op->aop = aop;
            freeAsmop(op,NULL,ic,TRUE);
            if (_G.r0Pushed) {
                emitcode("pop","ar0");
                _G.r0Pushed--;
            }

            if (_G.r1Pushed) {
                emitcode("pop","ar1");
                _G.r1Pushed--;
            }
        }
    }

dealloc:
    /* all other cases just dealloc */
    if (op ) {
        op->aop = NULL;
        if (IS_SYMOP(op)) {
            OP_SYMBOL(op)->aop = NULL;
            /* if the symbol has a spill */
      if (SPIL_LOC(op))
                SPIL_LOC(op)->aop = NULL;
        }
    }
}

/*-----------------------------------------------------------------*/
/* aopGet - for fetching value of the aop                          */
/*-----------------------------------------------------------------*/
static char *aopGet (asmop *aop, int offset, bool bit16, bool dname)
{
    char *s = buffer ;
    char *rs;

    //DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* offset is greater than
    size then zero */
    if (offset > (aop->size - 1) &&
        aop->type != AOP_LIT)
        return zero;

    /* depending on type */
    switch (aop->type) {

    case AOP_R0:
    case AOP_R1:
        DEBUGemitcode(";","%d",__LINE__);
  /* if we need to increment it */
  while (offset > aop->coff) {
      emitcode ("inc","%s",aop->aopu.aop_ptr->name);
      aop->coff++;
  }

  while (offset < aop->coff) {
      emitcode("dec","%s",aop->aopu.aop_ptr->name);
      aop->coff--;
  }

  aop->coff = offset ;
  if (aop->paged) {
      emitcode("movx","a,@%s",aop->aopu.aop_ptr->name);
      return (dname ? "acc" : "a");
  }
  sprintf(s,"@%s",aop->aopu.aop_ptr->name);
  rs = Safe_calloc(1,strlen(s)+1);
  strcpy(rs,s);
  return rs;

    case AOP_DPTR:
    case AOP_DPTR2:
        DEBUGemitcode(";","%d",__LINE__);
    if (aop->type == AOP_DPTR2)
    {
        genSetDPTR(1);
    }

  while (offset > aop->coff) {
      emitcode ("inc","dptr");
      aop->coff++;
  }

  while (offset < aop->coff) {
      emitcode("lcall","__decdptr");
      aop->coff--;
  }

  aop->coff = offset;
  if (aop->code) {
      emitcode("clr","a");
      emitcode("movc","a,@a+dptr");
        }
    else {
      emitcode("movx","a,@dptr");
    }

    if (aop->type == AOP_DPTR2)
    {
        genSetDPTR(0);
    }

    return (dname ? "acc" : "a");


    case AOP_IMMD:
      DEBUGemitcode(";","%d",__LINE__);
  if (bit16)
      sprintf (s,"%s",aop->aopu.aop_immd);
  else
      if (offset)
    sprintf(s,"(%s >> %d)",
      aop->aopu.aop_immd,
      offset*8);
      else
    sprintf(s,"%s",
      aop->aopu.aop_immd);
  rs = Safe_calloc(1,strlen(s)+1);
  strcpy(rs,s);
  return rs;

    case AOP_DIR:
  if (offset)
      sprintf(s,"(%s + %d)",
        aop->aopu.aop_dir,
        offset);
  else
      sprintf(s,"%s",aop->aopu.aop_dir);
  rs = Safe_calloc(1,strlen(s)+1);
  strcpy(rs,s);
  return rs;

    case AOP_REG:
      DEBUGemitcode(";","%d",__LINE__);
  if (dname)
      return aop->aopu.aop_reg[offset]->dname;
  else
      return aop->aopu.aop_reg[offset]->name;

    case AOP_CRY:
      emitcode(";","%d",__LINE__);
      //emitcode("clr","a");
      //emitcode("mov","c,%s",aop->aopu.aop_dir);
      //emitcode("rlc","a") ;
      //return (dname ? "acc" : "a");
      return aop->aopu.aop_dir;

    case AOP_ACC:
        DEBUGemitcode(";Warning -pic port ignoring get(AOP_ACC)","%d",__LINE__);
  //if (!offset && dname)
  //    return "acc";
  //return aop->aopu.aop_str[offset];
  return "AOP_accumulator_bug";

    case AOP_LIT:
        DEBUGemitcode(";","%d",__LINE__);
  return pic14aopLiteral (aop->aopu.aop_lit,offset);

    case AOP_STR:
        DEBUGemitcode(";","%d",__LINE__);
  aop->coff = offset ;
  if (strcmp(aop->aopu.aop_str[offset],"a") == 0 &&
      dname)
      return "acc";

  return aop->aopu.aop_str[offset];

    }

    werror(E_INTERNAL_ERROR,__FILE__,__LINE__,
           "aopget got unsupported aop->type");
    exit(0);
}
/*-----------------------------------------------------------------*/
/* aopPut - puts a string for a aop                                */
/*-----------------------------------------------------------------*/
static void aopPut (asmop *aop, char *s, int offset)
{
    char *d = buffer ;
    symbol *lbl ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    if (aop->size && offset > ( aop->size - 1)) {
        werror(E_INTERNAL_ERROR,__FILE__,__LINE__,
               "aopPut got offset > aop->size");
        exit(0);
    }

    /* will assign value to value */
    /* depending on where it is ofcourse */
    switch (aop->type) {
    case AOP_DIR:
  if (offset)
      sprintf(d,"(%s + %d)",
        aop->aopu.aop_dir,offset);
  else
      sprintf(d,"%s",aop->aopu.aop_dir);

  if (strcmp(d,s)) {
    DEBUGemitcode(";","%d",__LINE__);
    if(strcmp(s,"W"))
      emitcode("movf","%s,w",s);
    emitcode("movwf","%s",d);
  }
  break;

    case AOP_REG:
  if (strcmp(aop->aopu.aop_reg[offset]->name,s) != 0 &&
      strcmp(aop->aopu.aop_reg[offset]->dname,s)!= 0){
    /*
      if (*s == '@'           ||
    strcmp(s,"r0") == 0 ||
    strcmp(s,"r1") == 0 ||
    strcmp(s,"r2") == 0 ||
    strcmp(s,"r3") == 0 ||
    strcmp(s,"r4") == 0 ||
    strcmp(s,"r5") == 0 ||
    strcmp(s,"r6") == 0 ||
    strcmp(s,"r7") == 0 )
    emitcode("mov","%s,%s  ; %d",
       aop->aopu.aop_reg[offset]->dname,s,__LINE__);
      else
    */

    if(strcmp(s,"W"))
      emitcode("movf","%s,w  ; %d",s,__LINE__);

    emitcode("movwf","%s",
       aop->aopu.aop_reg[offset]->name);

  }
  break;

    case AOP_DPTR:
    case AOP_DPTR2:

    if (aop->type == AOP_DPTR2)
    {
        genSetDPTR(1);
    }

  if (aop->code) {
      werror(E_INTERNAL_ERROR,__FILE__,__LINE__,
       "aopPut writting to code space");
      exit(0);
  }

  while (offset > aop->coff) {
      aop->coff++;
      emitcode ("inc","dptr");
  }

  while (offset < aop->coff) {
      aop->coff-- ;
      emitcode("lcall","__decdptr");
  }

  aop->coff = offset;

  /* if not in accumulater */
  MOVA(s);

  emitcode ("movx","@dptr,a");

    if (aop->type == AOP_DPTR2)
    {
        genSetDPTR(0);
    }
  break;

    case AOP_R0:
    case AOP_R1:
  while (offset > aop->coff) {
      aop->coff++;
      emitcode("inc","%s",aop->aopu.aop_ptr->name);
  }
  while (offset < aop->coff) {
      aop->coff-- ;
      emitcode ("dec","%s",aop->aopu.aop_ptr->name);
  }
  aop->coff = offset;

  if (aop->paged) {
      MOVA(s);
      emitcode("movx","@%s,a",aop->aopu.aop_ptr->name);

  } else
      if (*s == '@') {
    MOVA(s);
    emitcode("mov","@%s,a ; %d",aop->aopu.aop_ptr->name,__LINE__);
      } else
    if (strcmp(s,"r0") == 0 ||
        strcmp(s,"r1") == 0 ||
        strcmp(s,"r2") == 0 ||
        strcmp(s,"r3") == 0 ||
        strcmp(s,"r4") == 0 ||
        strcmp(s,"r5") == 0 ||
        strcmp(s,"r6") == 0 ||
        strcmp(s,"r7") == 0 ) {
        char buffer[10];
        sprintf(buffer,"a%s",s);
        emitcode("mov","@%s,%s",
           aop->aopu.aop_ptr->name,buffer);
    } else
        emitcode("mov","@%s,%s",aop->aopu.aop_ptr->name,s);

  break;

    case AOP_STK:
  if (strcmp(s,"a") == 0)
      emitcode("push","acc");
  else
      emitcode("push","%s",s);

  break;

    case AOP_CRY:
  /* if bit variable */
  if (!aop->aopu.aop_dir) {
      emitcode("clr","a");
      emitcode("rlc","a");
  } else {
      if (s == zero)
    emitcode("clr","%s",aop->aopu.aop_dir);
      else
    if (s == one)
        emitcode("setb","%s",aop->aopu.aop_dir);
    else
        if (!strcmp(s,"c"))
      emitcode("mov","%s,c",aop->aopu.aop_dir);
        else {
      lbl = newiTempLabel(NULL);

      if (strcmp(s,"a")) {
          MOVA(s);
      }
      emitcode("clr","c");
      emitcode("jz","%05d_DS_",lbl->key+100);
      emitcode("cpl","c");
      emitcode("","%05d_DS_:",lbl->key+100);
      emitcode("mov","%s,c",aop->aopu.aop_dir);
        }
  }
  break;

    case AOP_STR:
  aop->coff = offset;
  if (strcmp(aop->aopu.aop_str[offset],s))
      emitcode ("mov","%s,%s ; %d",aop->aopu.aop_str[offset],s,__LINE__);
  break;

    case AOP_ACC:
  aop->coff = offset;
  if (!offset && (strcmp(s,"acc") == 0))
      break;

  if (strcmp(aop->aopu.aop_str[offset],s))
      emitcode ("mov","%s,%s ; %d",aop->aopu.aop_str[offset],s, __LINE__);
  break;

    default :
  werror(E_INTERNAL_ERROR,__FILE__,__LINE__,
         "aopPut got unsupported aop->type");
  exit(0);
    }

}

/*-----------------------------------------------------------------*/
/* reAdjustPreg - points a register back to where it should        */
/*-----------------------------------------------------------------*/
static void reAdjustPreg (asmop *aop)
{
    int size ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    aop->coff = 0;
    if ((size = aop->size) <= 1)
        return ;
    size-- ;
    switch (aop->type) {
        case AOP_R0 :
        case AOP_R1 :
            while (size--)
                emitcode("dec","%s",aop->aopu.aop_ptr->name);
            break;
        case AOP_DPTR :
        case AOP_DPTR2:
            if (aop->type == AOP_DPTR2)
          {
                genSetDPTR(1);
          }
            while (size--)
            {
                emitcode("lcall","__decdptr");
            }

          if (aop->type == AOP_DPTR2)
          {
                genSetDPTR(0);
          }
            break;

    }

}

#define AOP(op) op->aop
#define AOP_TYPE(op) AOP(op)->type
#define AOP_SIZE(op) AOP(op)->size
#define IS_AOP_PREG(x) (AOP(x) && (AOP_TYPE(x) == AOP_R1 || \
                       AOP_TYPE(x) == AOP_R0))

#define AOP_NEEDSACC(x) (AOP(x) && (AOP_TYPE(x) == AOP_CRY ||  \
                        AOP_TYPE(x) == AOP_DPTR || AOP_TYPE(x) == AOP_DPTR2 || \
                         AOP(x)->paged))

#define AOP_INPREG(x) (x && (x->type == AOP_REG &&                        \
                      (x->aopu.aop_reg[0] == pic14_regWithIdx(R0_IDX) || \
                      x->aopu.aop_reg[0] == pic14_regWithIdx(R1_IDX) )))

/*-----------------------------------------------------------------*/
/* genNotFloat - generates not for float operations              */
/*-----------------------------------------------------------------*/
static void genNotFloat (operand *op, operand *res)
{
    int size, offset;
    char *l;
    symbol *tlbl ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* we will put 127 in the first byte of
    the result */
    aopPut(AOP(res),"#127",0);
    size = AOP_SIZE(op) - 1;
    offset = 1;

    l = aopGet(op->aop,offset++,FALSE,FALSE);
    MOVA(l);

    while(size--) {
        emitcode("orl","a,%s",
                 aopGet(op->aop,
                        offset++,FALSE,FALSE));
    }
    tlbl = newiTempLabel(NULL);

    tlbl = newiTempLabel(NULL);
    aopPut(res->aop,one,1);
    emitcode("jz","%05d_DS_",(tlbl->key+100));
    aopPut(res->aop,zero,1);
    emitcode("","%05d_DS_:",(tlbl->key+100));

    size = res->aop->size - 2;
    offset = 2;
    /* put zeros in the rest */
    while (size--)
        aopPut(res->aop,zero,offset++);
}

#if 0
/*-----------------------------------------------------------------*/
/* opIsGptr: returns non-zero if the passed operand is       */
/* a generic pointer type.             */
/*-----------------------------------------------------------------*/
static int opIsGptr(operand *op)
{
    sym_link *type = operandType(op);

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if ((AOP_SIZE(op) == GPTRSIZE) && IS_GENPTR(type))
    {
        return 1;
    }
    return 0;
}
#endif

/*-----------------------------------------------------------------*/
/* getDataSize - get the operand data size                         */
/*-----------------------------------------------------------------*/
static int getDataSize(operand *op)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);


    return AOP_SIZE(op);

    // tsd- in the pic port, the genptr size is 1, so this code here
    // fails. ( in the 8051 port, the size was 4).
#if 0
    int size;
    size = AOP_SIZE(op);
    if (size == GPTRSIZE)
    {
        sym_link *type = operandType(op);
        if (IS_GENPTR(type))
        {
            /* generic pointer; arithmetic operations
             * should ignore the high byte (pointer type).
             */
            size--;
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
        }
    }
    return size;
#endif
}

/*-----------------------------------------------------------------*/
/* outAcc - output Acc                                             */
/*-----------------------------------------------------------------*/
static void outAcc(operand *result)
{
    int size, offset;
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    size = getDataSize(result);
    if(size){
        aopPut(AOP(result),"a",0);
        size--;
        offset = 1;
        /* unsigned or positive */
        while(size--){
            aopPut(AOP(result),zero,offset++);
        }
    }
}

/*-----------------------------------------------------------------*/
/* outBitC - output a bit C                                        */
/*-----------------------------------------------------------------*/
static void outBitC(operand *result)
{

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if the result is bit */
    if (AOP_TYPE(result) == AOP_CRY)
        aopPut(AOP(result),"c",0);
    else {
        emitcode("clr","a  ; %d", __LINE__);
        emitcode("rlc","a");
        outAcc(result);
    }
}

/*-----------------------------------------------------------------*/
/* toBoolean - emit code for orl a,operator(sizeop)                */
/*-----------------------------------------------------------------*/
static void toBoolean(operand *oper)
{
    int size = AOP_SIZE(oper) - 1;
    int offset = 1;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    if ( AOP_TYPE(oper) != AOP_ACC)
      emitcode("movf","%s,w",aopGet(AOP(oper),0,FALSE,FALSE));

    while (size--)
        emitcode("iorwf","%s,w",aopGet(AOP(oper),offset++,FALSE,FALSE));
}


/*-----------------------------------------------------------------*/
/* genNot - generate code for ! operation                          */
/*-----------------------------------------------------------------*/
static void genNot (iCode *ic)
{
    symbol *tlbl;
    sym_link *optype = operandType(IC_LEFT(ic));

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* assign asmOps to operand & result */
    aopOp (IC_LEFT(ic),ic,FALSE);
    aopOp (IC_RESULT(ic),ic,TRUE);

    /* if in bit space then a special case */
    if (AOP_TYPE(IC_LEFT(ic)) == AOP_CRY) {
      emitcode("movlw","1<<%s");
      //emitcode("mov","c,%s",IC_LEFT(ic)->aop->aopu.aop_dir);
      //emitcode("cpl","c");
      //outBitC(IC_RESULT(ic));
      goto release;
    }

    /* if type float then do float */
    if (IS_FLOAT(optype)) {
        genNotFloat(IC_LEFT(ic),IC_RESULT(ic));
        goto release;
    }

    toBoolean(IC_LEFT(ic));

    tlbl = newiTempLabel(NULL);
    emitcode("cjne","a,#0x01,%05d_DS_",tlbl->key+100);
    emitcode("","%05d_DS_:",tlbl->key+100);
    outBitC(IC_RESULT(ic));

release:
    /* release the aops */
    freeAsmop(IC_LEFT(ic),NULL,ic,(RESULTONSTACK(ic) ? 0 : 1));
    freeAsmop(IC_RESULT(ic),NULL,ic,TRUE);
}


/*-----------------------------------------------------------------*/
/* genCpl - generate code for complement                           */
/*-----------------------------------------------------------------*/
static void genCpl (iCode *ic)
{
    int offset = 0;
    int size ;


    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* assign asmOps to operand & result */
    aopOp (IC_LEFT(ic),ic,FALSE);
    aopOp (IC_RESULT(ic),ic,TRUE);

    /* if both are in bit space then
    a special case */
    if (AOP_TYPE(IC_RESULT(ic)) == AOP_CRY &&
        AOP_TYPE(IC_LEFT(ic)) == AOP_CRY ) {

        emitcode("mov","c,%s",IC_LEFT(ic)->aop->aopu.aop_dir);
        emitcode("cpl","c");
        emitcode("mov","%s,c",IC_RESULT(ic)->aop->aopu.aop_dir);
        goto release;
    }

    size = AOP_SIZE(IC_RESULT(ic));
    while (size--) {
        char *l = aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE);
        MOVA(l);
        emitcode("cpl","a");
        aopPut(AOP(IC_RESULT(ic)),"a",offset++);
    }


release:
    /* release the aops */
    freeAsmop(IC_LEFT(ic),NULL,ic,(RESULTONSTACK(ic) ? 0 : 1));
    freeAsmop(IC_RESULT(ic),NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genUminusFloat - unary minus for floating points                */
/*-----------------------------------------------------------------*/
static void genUminusFloat(operand *op,operand *result)
{
    int size ,offset =0 ;
    char *l;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* for this we just need to flip the
    first it then copy the rest in place */
    size = AOP_SIZE(op) - 1;
    l = aopGet(AOP(op),3,FALSE,FALSE);

    MOVA(l);

    emitcode("cpl","acc.7");
    aopPut(AOP(result),"a",3);

    while(size--) {
        aopPut(AOP(result),
               aopGet(AOP(op),offset,FALSE,FALSE),
               offset);
        offset++;
    }
}

/*-----------------------------------------------------------------*/
/* genUminus - unary minus code generation                         */
/*-----------------------------------------------------------------*/
static void genUminus (iCode *ic)
{
    int offset ,size ;
    sym_link *optype, *rtype;


    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* assign asmops */
    aopOp(IC_LEFT(ic),ic,FALSE);
    aopOp(IC_RESULT(ic),ic,TRUE);

    /* if both in bit space then special
    case */
    if (AOP_TYPE(IC_RESULT(ic)) == AOP_CRY &&
        AOP_TYPE(IC_LEFT(ic)) == AOP_CRY ) {

        emitcode("mov","c,%s",IC_LEFT(ic)->aop->aopu.aop_dir);
        emitcode("cpl","c");
        emitcode("mov","%s,c",IC_RESULT(ic)->aop->aopu.aop_dir);
        goto release;
    }

    optype = operandType(IC_LEFT(ic));
    rtype = operandType(IC_RESULT(ic));

    /* if float then do float stuff */
    if (IS_FLOAT(optype)) {
        genUminusFloat(IC_LEFT(ic),IC_RESULT(ic));
        goto release;
    }

    /* otherwise subtract from zero */
    size = AOP_SIZE(IC_LEFT(ic));
    offset = 0 ;
    CLRC ;
    while(size--) {
        char *l = aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE);
        if (!strcmp(l,"a")) {
            emitcode("cpl","a");
            emitcode("inc","a");
        } else {
            emitcode("clr","a");
            emitcode("subb","a,%s",l);
        }
        aopPut(AOP(IC_RESULT(ic)),"a",offset++);
    }

    /* if any remaining bytes in the result */
    /* we just need to propagate the sign   */
    if ((size = (AOP_SIZE(IC_RESULT(ic)) - AOP_SIZE(IC_LEFT(ic))))) {
        emitcode("rlc","a");
        emitcode("subb","a,acc");
        while (size--)
            aopPut(AOP(IC_RESULT(ic)),"a",offset++);
    }

release:
    /* release the aops */
    freeAsmop(IC_LEFT(ic),NULL,ic,(RESULTONSTACK(ic) ? 0 : 1));
    freeAsmop(IC_RESULT(ic),NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* saveRegisters - will look for a call and save the registers     */
/*-----------------------------------------------------------------*/
static void saveRegisters(iCode *lic)
{
    int i;
    iCode *ic;
    bitVect *rsave;
    sym_link *detype;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* look for call */
    for (ic = lic ; ic ; ic = ic->next)
        if (ic->op == CALL || ic->op == PCALL)
            break;

    if (!ic) {
        fprintf(stderr,"found parameter push with no function call\n");
        return ;
    }

    /* if the registers have been saved already then
    do nothing */
    if (ic->regsSaved || (OP_SYMBOL(IC_LEFT(ic))->calleeSave))
        return ;

    /* find the registers in use at this time
    and push them away to safety */
    rsave = bitVectCplAnd(bitVectCopy(ic->rMask),
                          ic->rUsed);

    ic->regsSaved = 1;
    if (options.useXstack) {
  if (bitVectBitValue(rsave,R0_IDX))
      emitcode("mov","b,r0");
  emitcode("mov","r0,%s",spname);
  for (i = 0 ; i < pic14_nRegs ; i++) {
      if (bitVectBitValue(rsave,i)) {
    if (i == R0_IDX)
        emitcode("mov","a,b");
    else
        emitcode("mov","a,%s",pic14_regWithIdx(i)->name);
    emitcode("movx","@r0,a");
    emitcode("inc","r0");
      }
  }
  emitcode("mov","%s,r0",spname);
  if (bitVectBitValue(rsave,R0_IDX))
      emitcode("mov","r0,b");
    } else
  for (i = 0 ; i < pic14_nRegs ; i++) {
      if (bitVectBitValue(rsave,i))
    emitcode("push","%s",pic14_regWithIdx(i)->dname);
  }

    detype = getSpec(operandType(IC_LEFT(ic)));
    if (detype        &&
        (SPEC_BANK(currFunc->etype) != SPEC_BANK(detype)) &&
  IS_ISR(currFunc->etype) &&
        !ic->bankSaved)

        saverbank(SPEC_BANK(detype),ic,TRUE);

}
/*-----------------------------------------------------------------*/
/* unsaveRegisters - pop the pushed registers                      */
/*-----------------------------------------------------------------*/
static void unsaveRegisters (iCode *ic)
{
    int i;
    bitVect *rsave;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* find the registers in use at this time
    and push them away to safety */
    rsave = bitVectCplAnd(bitVectCopy(ic->rMask),
                          ic->rUsed);

    if (options.useXstack) {
  emitcode("mov","r0,%s",spname);
  for (i =  pic14_nRegs ; i >= 0 ; i--) {
      if (bitVectBitValue(rsave,i)) {
    emitcode("dec","r0");
    emitcode("movx","a,@r0");
    if (i == R0_IDX)
        emitcode("mov","b,a");
    else
        emitcode("mov","%s,a",pic14_regWithIdx(i)->name);
      }

  }
  emitcode("mov","%s,r0",spname);
  if (bitVectBitValue(rsave,R0_IDX))
      emitcode("mov","r0,b");
    } else
  for (i =  pic14_nRegs ; i >= 0 ; i--) {
      if (bitVectBitValue(rsave,i))
    emitcode("pop","%s",pic14_regWithIdx(i)->dname);
  }

}


/*-----------------------------------------------------------------*/
/* pushSide -                */
/*-----------------------------------------------------------------*/
static void pushSide(operand * oper, int size)
{
  int offset = 0;
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
  while (size--) {
    char *l = aopGet(AOP(oper),offset++,FALSE,TRUE);
    if (AOP_TYPE(oper) != AOP_REG &&
        AOP_TYPE(oper) != AOP_DIR &&
        strcmp(l,"a") ) {
      emitcode("mov","a,%s",l);
      emitcode("push","acc");
    } else
      emitcode("push","%s",l);
  }
}

/*-----------------------------------------------------------------*/
/* assignResultValue -               */
/*-----------------------------------------------------------------*/
static void assignResultValue(operand * oper)
{
  int offset = 0;
  int size = AOP_SIZE(oper);

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    // The last byte in the assignment is in W
    aopPut(AOP(oper),"W",size-1);

    if(size>1) {
      while (--size) {
  aopPut(AOP(oper),fReturn[offset],offset);
  offset++;

      }
    }
}


/*-----------------------------------------------------------------*/
/* genXpush - pushes onto the external stack                       */
/*-----------------------------------------------------------------*/
static void genXpush (iCode *ic)
{
    asmop *aop = newAsmop(0);
    regs *r ;
    int size,offset = 0;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    aopOp(IC_LEFT(ic),ic,FALSE);
    r = getFreePtr(ic,&aop,FALSE);


    emitcode("mov","%s,_spx",r->name);

    size = AOP_SIZE(IC_LEFT(ic));
    while(size--) {

  char *l = aopGet(AOP(IC_LEFT(ic)),
       offset++,FALSE,FALSE);
  MOVA(l);
  emitcode("movx","@%s,a",r->name);
  emitcode("inc","%s",r->name);

    }


    emitcode("mov","_spx,%s",r->name);

    freeAsmop(NULL,aop,ic,TRUE);
    freeAsmop(IC_LEFT(ic),NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genIpush - genrate code for pushing this gets a little complex  */
/*-----------------------------------------------------------------*/
static void genIpush (iCode *ic)
{
    int size, offset = 0 ;
    char *l;


    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if this is not a parm push : ie. it is spill push
    and spill push is always done on the local stack */
    if (!ic->parmPush) {

        /* and the item is spilt then do nothing */
        if (OP_SYMBOL(IC_LEFT(ic))->isspilt)
            return ;

        aopOp(IC_LEFT(ic),ic,FALSE);
        size = AOP_SIZE(IC_LEFT(ic));
        /* push it on the stack */
        while(size--) {
            l = aopGet(AOP(IC_LEFT(ic)),offset++,FALSE,TRUE);
            if (*l == '#') {
                MOVA(l);
                l = "acc";
            }
            emitcode("push","%s",l);
        }
        return ;
    }

    /* this is a paramter push: in this case we call
    the routine to find the call and save those
    registers that need to be saved */
    saveRegisters(ic);

    /* if use external stack then call the external
    stack pushing routine */
    if (options.useXstack) {
        genXpush(ic);
        return ;
    }

    /* then do the push */
    aopOp(IC_LEFT(ic),ic,FALSE);


  // pushSide(IC_LEFT(ic), AOP_SIZE(IC_LEFT(ic)));
    size = AOP_SIZE(IC_LEFT(ic));

    while (size--) {
        l = aopGet(AOP(IC_LEFT(ic)),offset++,FALSE,TRUE);
        if (AOP_TYPE(IC_LEFT(ic)) != AOP_REG &&
            AOP_TYPE(IC_LEFT(ic)) != AOP_DIR &&
            strcmp(l,"a") ) {
            emitcode("mov","a,%s",l);
            emitcode("push","acc");
        } else
            emitcode("push","%s",l);
    }

    freeAsmop(IC_LEFT(ic),NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genIpop - recover the registers: can happen only for spilling   */
/*-----------------------------------------------------------------*/
static void genIpop (iCode *ic)
{
    int size,offset ;


    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if the temp was not pushed then */
    if (OP_SYMBOL(IC_LEFT(ic))->isspilt)
        return ;

    aopOp(IC_LEFT(ic),ic,FALSE);
    size = AOP_SIZE(IC_LEFT(ic));
    offset = (size-1);
    while (size--)
        emitcode("pop","%s",aopGet(AOP(IC_LEFT(ic)),offset--,
                                   FALSE,TRUE));

    freeAsmop(IC_LEFT(ic),NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* unsaverbank - restores the resgister bank from stack            */
/*-----------------------------------------------------------------*/
static void unsaverbank (int bank,iCode *ic,bool popPsw)
{
    int i;
    asmop *aop ;
    regs *r = NULL;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if (popPsw) {
  if (options.useXstack) {
      aop = newAsmop(0);
      r = getFreePtr(ic,&aop,FALSE);


      emitcode("mov","%s,_spx",r->name);
      emitcode("movx","a,@%s",r->name);
      emitcode("mov","psw,a");
      emitcode("dec","%s",r->name);

  }else
      emitcode ("pop","psw");
    }

    for (i = (pic14_nRegs - 1) ; i >= 0 ;i--) {
        if (options.useXstack) {
            emitcode("movx","a,@%s",r->name);
            //emitcode("mov","(%s+%d),a",
      //       regspic14[i].base,8*bank+regspic14[i].offset);
            emitcode("dec","%s",r->name);

        } else
    emitcode("pop",""); //"(%s+%d)",
  //regspic14[i].base,8*bank); //+regspic14[i].offset);
    }

    if (options.useXstack) {

  emitcode("mov","_spx,%s",r->name);
  freeAsmop(NULL,aop,ic,TRUE);

    }
}

/*-----------------------------------------------------------------*/
/* saverbank - saves an entire register bank on the stack          */
/*-----------------------------------------------------------------*/
static void saverbank (int bank, iCode *ic, bool pushPsw)
{
    int i;
    asmop *aop ;
    regs *r = NULL;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if (options.useXstack) {

  aop = newAsmop(0);
  r = getFreePtr(ic,&aop,FALSE);
  emitcode("mov","%s,_spx",r->name);

    }

    for (i = 0 ; i < pic14_nRegs ;i++) {
        if (options.useXstack) {
            emitcode("inc","%s",r->name);
            //emitcode("mov","a,(%s+%d)",
            //         regspic14[i].base,8*bank+regspic14[i].offset);
            emitcode("movx","@%s,a",r->name);
        } else
    emitcode("push","");// "(%s+%d)",
                     //regspic14[i].base,8*bank+regspic14[i].offset);
    }

    if (pushPsw) {
  if (options.useXstack) {
      emitcode("mov","a,psw");
      emitcode("movx","@%s,a",r->name);
      emitcode("inc","%s",r->name);
      emitcode("mov","_spx,%s",r->name);
      freeAsmop (NULL,aop,ic,TRUE);

  } else
      emitcode("push","psw");

  emitcode("mov","psw,#0x%02x",(bank << 3)&0x00ff);
    }
    ic->bankSaved = 1;

}

/*-----------------------------------------------------------------*/
/* genCall - generates a call statement                            */
/*-----------------------------------------------------------------*/
static void genCall (iCode *ic)
{
    sym_link *detype;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    /* if caller saves & we have not saved then */
    if (!ic->regsSaved)
        saveRegisters(ic);

    /* if we are calling a function that is not using
    the same register bank then we need to save the
    destination registers on the stack */
    detype = getSpec(operandType(IC_LEFT(ic)));
    if (detype        &&
        (SPEC_BANK(currFunc->etype) != SPEC_BANK(detype)) &&
  IS_ISR(currFunc->etype) &&
        !ic->bankSaved)

        saverbank(SPEC_BANK(detype),ic,TRUE);

    /* if send set is not empty the assign */
    if (_G.sendSet) {
  iCode *sic ;

  for (sic = setFirstItem(_G.sendSet) ; sic ;
       sic = setNextItem(_G.sendSet)) {
      int size, offset = 0;

      aopOp(IC_LEFT(sic),sic,FALSE);
      size = AOP_SIZE(IC_LEFT(sic));
      while (size--) {
    char *l = aopGet(AOP(IC_LEFT(sic)),offset,
        FALSE,FALSE);
    DEBUGemitcode(";","%d - left type %d",__LINE__,AOP(IC_LEFT(sic))->type);

    if (strcmp(l,fReturn[offset])) {

      if ( ((AOP(IC_LEFT(sic))->type) == AOP_IMMD) ||
           ((AOP(IC_LEFT(sic))->type) == AOP_LIT) )
        emitcode("movlw","%s",l);
      else
        emitcode("movf","%s,w",l);
      // The last one is past in W
      if(size)
        emitcode("movwf","%s",fReturn[offset]);
    }
    offset++;
      }
      freeAsmop (IC_LEFT(sic),NULL,sic,TRUE);
  }
  _G.sendSet = NULL;
    }
    /* make the call */
    emitcode("call","%s",(OP_SYMBOL(IC_LEFT(ic))->rname[0] ?
                           OP_SYMBOL(IC_LEFT(ic))->rname :
                           OP_SYMBOL(IC_LEFT(ic))->name));

    /* if we need assign a result value */
    if ((IS_ITEMP(IC_RESULT(ic)) &&
         (OP_SYMBOL(IC_RESULT(ic))->nRegs ||
          OP_SYMBOL(IC_RESULT(ic))->spildir )) ||
        IS_TRUE_SYMOP(IC_RESULT(ic)) ) {

        _G.accInUse++;
        aopOp(IC_RESULT(ic),ic,FALSE);
        _G.accInUse--;

  assignResultValue(IC_RESULT(ic));

        freeAsmop(IC_RESULT(ic),NULL, ic,TRUE);
    }

    /* adjust the stack for parameters if
    required */
    if (IC_LEFT(ic)->parmBytes) {
        int i;
        if (IC_LEFT(ic)->parmBytes > 3) {
            emitcode("mov","a,%s",spname);
            emitcode("add","a,#0x%02x", (- IC_LEFT(ic)->parmBytes) & 0xff);
            emitcode("mov","%s,a",spname);
        } else
            for ( i = 0 ; i <  IC_LEFT(ic)->parmBytes ;i++)
                emitcode("dec","%s",spname);

    }

    /* if register bank was saved then pop them */
    if (ic->bankSaved)
        unsaverbank(SPEC_BANK(detype),ic,TRUE);

    /* if we hade saved some registers then unsave them */
    if (ic->regsSaved && !(OP_SYMBOL(IC_LEFT(ic))->calleeSave))
        unsaveRegisters (ic);


}

/*-----------------------------------------------------------------*/
/* genPcall - generates a call by pointer statement                */
/*-----------------------------------------------------------------*/
static void genPcall (iCode *ic)
{
    sym_link *detype;
    symbol *rlbl = newiTempLabel(NULL);


    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if caller saves & we have not saved then */
    if (!ic->regsSaved)
        saveRegisters(ic);

    /* if we are calling a function that is not using
    the same register bank then we need to save the
    destination registers on the stack */
    detype = getSpec(operandType(IC_LEFT(ic)));
    if (detype        &&
  IS_ISR(currFunc->etype) &&
        (SPEC_BANK(currFunc->etype) != SPEC_BANK(detype)))
        saverbank(SPEC_BANK(detype),ic,TRUE);


    /* push the return address on to the stack */
    emitcode("mov","a,#%05d_DS_",(rlbl->key+100));
    emitcode("push","acc");
    emitcode("mov","a,#(%05d_DS_ >> 8)",(rlbl->key+100));
    emitcode("push","acc");

    if (options.model == MODEL_FLAT24)
    {
      emitcode("mov","a,#(%05d_DS_ >> 16)",(rlbl->key+100));
      emitcode("push","acc");
    }

    /* now push the calling address */
    aopOp(IC_LEFT(ic),ic,FALSE);

    pushSide(IC_LEFT(ic), FPTRSIZE);

    freeAsmop(IC_LEFT(ic),NULL,ic,TRUE);

    /* if send set is not empty the assign */
    if (_G.sendSet) {
  iCode *sic ;

  for (sic = setFirstItem(_G.sendSet) ; sic ;
       sic = setNextItem(_G.sendSet)) {
      int size, offset = 0;
      aopOp(IC_LEFT(sic),sic,FALSE);
      size = AOP_SIZE(IC_LEFT(sic));
      while (size--) {
    char *l = aopGet(AOP(IC_LEFT(sic)),offset,
        FALSE,FALSE);
    if (strcmp(l,fReturn[offset]))
        emitcode("mov","%s,%s",
           fReturn[offset],
           l);
    offset++;
      }
      freeAsmop (IC_LEFT(sic),NULL,sic,TRUE);
  }
  _G.sendSet = NULL;
    }

    emitcode("ret","");
    emitcode("","%05d_DS_:",(rlbl->key+100));


    /* if we need assign a result value */
    if ((IS_ITEMP(IC_RESULT(ic)) &&
         (OP_SYMBOL(IC_RESULT(ic))->nRegs ||
          OP_SYMBOL(IC_RESULT(ic))->spildir)) ||
        IS_TRUE_SYMOP(IC_RESULT(ic)) ) {

        _G.accInUse++;
        aopOp(IC_RESULT(ic),ic,FALSE);
        _G.accInUse--;

  assignResultValue(IC_RESULT(ic));

        freeAsmop(IC_RESULT(ic),NULL,ic,TRUE);
    }

    /* adjust the stack for parameters if
    required */
    if (IC_LEFT(ic)->parmBytes) {
        int i;
        if (IC_LEFT(ic)->parmBytes > 3) {
            emitcode("mov","a,%s",spname);
            emitcode("add","a,#0x%02x", (- IC_LEFT(ic)->parmBytes) & 0xff);
            emitcode("mov","%s,a",spname);
        } else
            for ( i = 0 ; i <  IC_LEFT(ic)->parmBytes ;i++)
                emitcode("dec","%s",spname);

    }

    /* if register bank was saved then unsave them */
    if (detype        &&
        (SPEC_BANK(currFunc->etype) !=
         SPEC_BANK(detype)))
        unsaverbank(SPEC_BANK(detype),ic,TRUE);

    /* if we hade saved some registers then
    unsave them */
    if (ic->regsSaved)
        unsaveRegisters (ic);

}

/*-----------------------------------------------------------------*/
/* resultRemat - result  is rematerializable                       */
/*-----------------------------------------------------------------*/
static int resultRemat (iCode *ic)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if (SKIP_IC(ic) || ic->op == IFX)
        return 0;

    if (IC_RESULT(ic) && IS_ITEMP(IC_RESULT(ic))) {
        symbol *sym = OP_SYMBOL(IC_RESULT(ic));
        if (sym->remat && !POINTER_SET(ic))
            return 1;
    }

    return 0;
}

#if defined(__BORLANDC__) || defined(_MSC_VER)
#define STRCASECMP stricmp
#else
#define STRCASECMP strcasecmp
#endif

/*-----------------------------------------------------------------*/
/* inExcludeList - return 1 if the string is in exclude Reg list   */
/*-----------------------------------------------------------------*/
static bool inExcludeList(char *s)
{
    int i =0;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if (options.excludeRegs[i] &&
    STRCASECMP(options.excludeRegs[i],"none") == 0)
  return FALSE ;

    for ( i = 0 ; options.excludeRegs[i]; i++) {
  if (options.excludeRegs[i] &&
        STRCASECMP(s,options.excludeRegs[i]) == 0)
      return TRUE;
    }
    return FALSE ;
}

/*-----------------------------------------------------------------*/
/* genFunction - generated code for function entry                 */
/*-----------------------------------------------------------------*/
static void genFunction (iCode *ic)
{
    symbol *sym;
    sym_link *fetype;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    labelOffset += FUNCTION_LABEL_INC;

    _G.nRegsSaved = 0;
    /* create the function header */
    emitcode(";","-----------------------------------------");
    emitcode(";"," function %s",(sym = OP_SYMBOL(IC_LEFT(ic)))->name);
    emitcode(";","-----------------------------------------");

    emitcode("","%s:",sym->rname);
    fetype = getSpec(operandType(IC_LEFT(ic)));

    /* if critical function then turn interrupts off */
    if (SPEC_CRTCL(fetype))
        emitcode("clr","ea");

    /* here we need to generate the equates for the
       register bank if required */
#if 0
    if (SPEC_BANK(fetype) != rbank) {
        int i ;

        rbank = SPEC_BANK(fetype);
        for ( i = 0 ; i < pic14_nRegs ; i++ ) {
            if (strcmp(regspic14[i].base,"0") == 0)
                emitcode("","%s = 0x%02x",
                         regspic14[i].dname,
                         8*rbank+regspic14[i].offset);
            else
                emitcode ("","%s = %s + 0x%02x",
                          regspic14[i].dname,
                          regspic14[i].base,
                          8*rbank+regspic14[i].offset);
        }
    }
#endif

    /* if this is an interrupt service routine then
    save acc, b, dpl, dph  */
    if (IS_ISR(sym->etype)) {

  if (!inExcludeList("acc"))
      emitcode ("push","acc");
  if (!inExcludeList("b"))
      emitcode ("push","b");
  if (!inExcludeList("dpl"))
      emitcode ("push","dpl");
  if (!inExcludeList("dph"))
      emitcode ("push","dph");
  if (options.model == MODEL_FLAT24 && !inExcludeList("dpx"))
  {
      emitcode ("push", "dpx");
      /* Make sure we're using standard DPTR */
      emitcode ("push", "dps");
      emitcode ("mov", "dps, #0x00");
      if (options.stack10bit)
      {
        /* This ISR could conceivably use DPTR2. Better save it. */
        emitcode ("push", "dpl1");
        emitcode ("push", "dph1");
        emitcode ("push", "dpx1");
      }
  }
  /* if this isr has no bank i.e. is going to
     run with bank 0 , then we need to save more
     registers :-) */
  if (!SPEC_BANK(sym->etype)) {

      /* if this function does not call any other
         function then we can be economical and
         save only those registers that are used */
      if (! sym->hasFcall) {
    int i;

    /* if any registers used */
    if (sym->regsUsed) {
        /* save the registers used */
        for ( i = 0 ; i < sym->regsUsed->size ; i++) {
      if (bitVectBitValue(sym->regsUsed,i) ||
                          (pic14_ptrRegReq && (i == R0_IDX || i == R1_IDX)) )
          emitcode("push","%s",pic14_regWithIdx(i)->dname);
        }
    }

      } else {
    /* this function has  a function call cannot
       determines register usage so we will have the
       entire bank */
    saverbank(0,ic,FALSE);
      }
  }
    } else {
  /* if callee-save to be used for this function
     then save the registers being used in this function */
  if (sym->calleeSave) {
      int i;

      /* if any registers used */
      if (sym->regsUsed) {
    /* save the registers used */
    for ( i = 0 ; i < sym->regsUsed->size ; i++) {
        if (bitVectBitValue(sym->regsUsed,i) ||
                      (pic14_ptrRegReq && (i == R0_IDX || i == R1_IDX)) ) {
      emitcode("push","%s",pic14_regWithIdx(i)->dname);
      _G.nRegsSaved++;
        }
    }
      }
  }
    }

    /* set the register bank to the desired value */
    if (SPEC_BANK(sym->etype) || IS_ISR(sym->etype)) {
        emitcode("push","psw");
        emitcode("mov","psw,#0x%02x",(SPEC_BANK(sym->etype) << 3)&0x00ff);
    }

    if (IS_RENT(sym->etype) || options.stackAuto) {

  if (options.useXstack) {
      emitcode("mov","r0,%s",spname);
      emitcode("mov","a,_bp");
      emitcode("movx","@r0,a");
      emitcode("inc","%s",spname);
  }
  else
  {
      /* set up the stack */
      emitcode ("push","_bp");     /* save the callers stack  */
  }
  emitcode ("mov","_bp,%s",spname);
    }

    /* adjust the stack for the function */
    if (sym->stack) {

  int i = sym->stack;
  if (i > 256 )
      werror(W_STACK_OVERFLOW,sym->name);

  if (i > 3 && sym->recvSize < 4) {

      emitcode ("mov","a,sp");
      emitcode ("add","a,#0x%02x",((char)sym->stack & 0xff));
      emitcode ("mov","sp,a");

  }
  else
      while(i--)
    emitcode("inc","sp");
    }

     if (sym->xstack) {

  emitcode ("mov","a,_spx");
  emitcode ("add","a,#0x%02x",((char)sym->xstack & 0xff));
  emitcode ("mov","_spx,a");
    }

}

/*-----------------------------------------------------------------*/
/* genEndFunction - generates epilogue for functions               */
/*-----------------------------------------------------------------*/
static void genEndFunction (iCode *ic)
{
    symbol *sym = OP_SYMBOL(IC_LEFT(ic));

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    if (IS_RENT(sym->etype) || options.stackAuto)
    {
        emitcode ("mov","%s,_bp",spname);
    }

    /* if use external stack but some variables were
    added to the local stack then decrement the
    local stack */
    if (options.useXstack && sym->stack) {
        emitcode("mov","a,sp");
        emitcode("add","a,#0x%02x",((char)-sym->stack) & 0xff);
        emitcode("mov","sp,a");
    }


    if ((IS_RENT(sym->etype) || options.stackAuto)) {
  if (options.useXstack) {
      emitcode("mov","r0,%s",spname);
      emitcode("movx","a,@r0");
      emitcode("mov","_bp,a");
      emitcode("dec","%s",spname);
  }
  else
  {
      emitcode ("pop","_bp");
  }
    }

    /* restore the register bank  */
    if (SPEC_BANK(sym->etype) || IS_ISR(sym->etype))
        emitcode ("pop","psw");

    if (IS_ISR(sym->etype)) {

  /* now we need to restore the registers */
  /* if this isr has no bank i.e. is going to
     run with bank 0 , then we need to save more
     registers :-) */
  if (!SPEC_BANK(sym->etype)) {

      /* if this function does not call any other
         function then we can be economical and
         save only those registers that are used */
      if (! sym->hasFcall) {
    int i;

    /* if any registers used */
    if (sym->regsUsed) {
        /* save the registers used */
        for ( i = sym->regsUsed->size ; i >= 0 ; i--) {
      if (bitVectBitValue(sym->regsUsed,i) ||
                          (pic14_ptrRegReq && (i == R0_IDX || i == R1_IDX)) )
          emitcode("pop","%s",pic14_regWithIdx(i)->dname);
        }
    }

      } else {
    /* this function has  a function call cannot
       determines register usage so we will have the
       entire bank */
    unsaverbank(0,ic,FALSE);
      }
  }

  if (options.model == MODEL_FLAT24 && !inExcludeList("dpx"))
  {
      if (options.stack10bit)
      {
          emitcode ("pop", "dpx1");
          emitcode ("pop", "dph1");
          emitcode ("pop", "dpl1");
      }
      emitcode ("pop", "dps");
      emitcode ("pop", "dpx");
  }
  if (!inExcludeList("dph"))
      emitcode ("pop","dph");
  if (!inExcludeList("dpl"))
      emitcode ("pop","dpl");
  if (!inExcludeList("b"))
      emitcode ("pop","b");
  if (!inExcludeList("acc"))
      emitcode ("pop","acc");

        if (SPEC_CRTCL(sym->etype))
            emitcode("setb","ea");

  /* if debug then send end of function */
/*  if (options.debug && currFunc) { */
  if (currFunc) {
      _G.debugLine = 1;
      emitcode(";","C$%s$%d$%d$%d ==.",
         ic->filename,currFunc->lastLine,
         ic->level,ic->block);
      if (IS_STATIC(currFunc->etype))
    emitcode(";","XF%s$%s$0$0 ==.",moduleName,currFunc->name);
      else
    emitcode(";","XG$%s$0$0 ==.",currFunc->name);
      _G.debugLine = 0;
  }

        emitcode ("reti","");
    }
    else {
        if (SPEC_CRTCL(sym->etype))
            emitcode("setb","ea");

  if (sym->calleeSave) {
      int i;

      /* if any registers used */
      if (sym->regsUsed) {
    /* save the registers used */
    for ( i = sym->regsUsed->size ; i >= 0 ; i--) {
        if (bitVectBitValue(sym->regsUsed,i) ||
                      (pic14_ptrRegReq && (i == R0_IDX || i == R1_IDX)) )
      emitcode("pop","%s",pic14_regWithIdx(i)->dname);
    }
      }

  }

  /* if debug then send end of function */
  if (currFunc) {
      _G.debugLine = 1;
      emitcode(";","C$%s$%d$%d$%d ==.",
         ic->filename,currFunc->lastLine,
         ic->level,ic->block);
      if (IS_STATIC(currFunc->etype))
    emitcode(";","XF%s$%s$0$0 ==.",moduleName,currFunc->name);
      else
    emitcode(";","XG$%s$0$0 ==.",currFunc->name);
      _G.debugLine = 0;
  }

        emitcode ("return","");
    }

}

/*-----------------------------------------------------------------*/
/* genRet - generate code for return statement                     */
/*-----------------------------------------------------------------*/
static void genRet (iCode *ic)
{
    int size,offset = 0 , pushed = 0;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if we have no return value then
       just generate the "ret" */
    if (!IC_LEFT(ic))
  goto jumpret;

    /* we have something to return then
       move the return value into place */
    aopOp(IC_LEFT(ic),ic,FALSE);
    size = AOP_SIZE(IC_LEFT(ic));

    while (size--) {
      char *l ;
      if (AOP_TYPE(IC_LEFT(ic)) == AOP_DPTR) {
            /* #NOCHANGE */
        l = aopGet(AOP(IC_LEFT(ic)),offset++,
         FALSE,TRUE);
        emitcode("push","%s",l);
        pushed++;
      } else {
        l = aopGet(AOP(IC_LEFT(ic)),offset,
             FALSE,FALSE);
        if (strcmp(fReturn[offset],l)) {
          if( ( (AOP(IC_LEFT(ic))->type) == AOP_IMMD) ||
        ((AOP(IC_LEFT(ic))->type) == AOP_LIT) )
      emitcode("movlw","%s",l);
          else
      emitcode("movf","%s,w",l);
          if(size)
      emitcode("movwf","%s",fReturn[offset]);
          offset++;
        }
      }
    }

    if (pushed) {
  while(pushed) {
      pushed--;
      if (strcmp(fReturn[pushed],"a"))
    emitcode("pop",fReturn[pushed]);
      else
    emitcode("pop","acc");
  }
    }
    freeAsmop (IC_LEFT(ic),NULL,ic,TRUE);

 jumpret:
  /* generate a jump to the return label
     if the next is not the return statement */
    if (!(ic->next && ic->next->op == LABEL &&
    IC_LABEL(ic->next) == returnLabel))

  emitcode("goto","_%05d_DS_",returnLabel->key+100 + labelOffset);

}

/*-----------------------------------------------------------------*/
/* genLabel - generates a label                                    */
/*-----------------------------------------------------------------*/
static void genLabel (iCode *ic)
{
    /* special case never generate */
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if (IC_LABEL(ic) == entryLabel)
        return ;

    emitcode("","_%05d_DS_:",(IC_LABEL(ic)->key+100 + labelOffset));
}

/*-----------------------------------------------------------------*/
/* genGoto - generates a goto                                      */
/*-----------------------------------------------------------------*/
//tsd
static void genGoto (iCode *ic)
{
    emitcode ("goto","_%05d_DS_",(IC_LABEL(ic)->key+100)+labelOffset);
}

/*-----------------------------------------------------------------*/
/* findLabelBackwards: walks back through the iCode chain looking  */
/* for the given label. Returns number of iCode instructions     */
/* between that label and given ic.          */
/* Returns zero if label not found.          */
/*-----------------------------------------------------------------*/
#if 0
static int findLabelBackwards(iCode *ic, int key)
{
    int count = 0;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    while (ic->prev)
    {
        ic = ic->prev;
        count++;

        if (ic->op == LABEL && IC_LABEL(ic)->key == key)
        {
            /* printf("findLabelBackwards = %d\n", count); */
            return count;
        }
    }

    return 0;
}
#endif
/*-----------------------------------------------------------------*/
/* genPlusIncr :- does addition with increment if possible         */
/*-----------------------------------------------------------------*/
static bool genPlusIncr (iCode *ic)
{
    unsigned int icount ;
    unsigned int size = getDataSize(IC_RESULT(ic));

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    DEBUGemitcode ("; ","result %d, left %d, right %d",
       AOP_TYPE(IC_RESULT(ic)),
       AOP_TYPE(IC_LEFT(ic)),
       AOP_TYPE(IC_RIGHT(ic)));

    /* will try to generate an increment */
    /* if the right side is not a literal
       we cannot */
    if (AOP_TYPE(IC_RIGHT(ic)) != AOP_LIT)
        return FALSE ;

    DEBUGemitcode ("; ","%s  %d",__FUNCTION__,__LINE__);
    /* if the literal value of the right hand side
       is greater than 1 then it is faster to add */
    if ((icount =  floatFromVal (AOP(IC_RIGHT(ic))->aopu.aop_lit)) > 2)
        return FALSE ;

    /* if increment 16 bits in register */
    if (sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))) &&
        (icount == 1)) {

      int offset = MSB16;

      emitcode("incf","%s,f",aopGet(AOP(IC_RESULT(ic)),LSB,FALSE,FALSE));

      while(--size) {
  emitSKPNZ;
  emitcode(" incf","%s,f",aopGet(AOP(IC_RESULT(ic)),offset++,FALSE,FALSE));
      }

      return TRUE;
    }

    DEBUGemitcode ("; ","%s  %d",__FUNCTION__,__LINE__);
    /* if left is in accumulator  - probably a bit operation*/
    if( strcmp(aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE),"a")  &&
  (AOP_TYPE(IC_RESULT(ic)) == AOP_CRY) ) {

      emitcode("bcf","(%s >> 3), (%s & 7)",
         AOP(IC_RESULT(ic))->aopu.aop_dir,
         AOP(IC_RESULT(ic))->aopu.aop_dir);
      if(icount)
  emitcode("xorlw","1");
      else
  emitcode("andlw","1");

      emitSKPZ;
      emitcode("bsf","(%s >> 3), (%s & 7)",
         AOP(IC_RESULT(ic))->aopu.aop_dir,
         AOP(IC_RESULT(ic))->aopu.aop_dir);

      return TRUE;
    }



    /* if the sizes are greater than 1 then we cannot */
    if (AOP_SIZE(IC_RESULT(ic)) > 1 ||
        AOP_SIZE(IC_LEFT(ic)) > 1   )
        return FALSE ;

    /* If we are incrementing the same register by two: */

    if (sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))) ) {

      while (icount--)
  emitcode("incf","%s,f",aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));

      return TRUE ;
    }

    DEBUGemitcode ("; ","couldn't increment result-%s  left-%s",
       aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE),
       aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
    return FALSE ;
}

/*-----------------------------------------------------------------*/
/* outBitAcc - output a bit in acc                                 */
/*-----------------------------------------------------------------*/
static void outBitAcc(operand *result)
{
    symbol *tlbl = newiTempLabel(NULL);
    /* if the result is a bit */
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    if (AOP_TYPE(result) == AOP_CRY){
        aopPut(AOP(result),"a",0);
    }
    else {
        emitcode("jz","%05d_DS_",tlbl->key+100);
        emitcode("mov","a,%s",one);
        emitcode("","%05d_DS_:",tlbl->key+100);
        outAcc(result);
    }
}

/*-----------------------------------------------------------------*/
/* genPlusBits - generates code for addition of two bits           */
/*-----------------------------------------------------------------*/
static void genPlusBits (iCode *ic)
{

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /*
      The following block of code will add two bits.
      Note that it'll even work if the destination is
      the carry (C in the status register).
      It won't work if the 'Z' bit is a source or destination.
    */

    /* If the result is stored in the accumulator (w) */
    if(strcmp(aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE),"a") == 0 ) {
  emitcode("movlw","(1 << (%s & 7))",
     AOP(IC_RESULT(ic))->aopu.aop_dir,
     AOP(IC_RESULT(ic))->aopu.aop_dir);
  emitcode("bcf","(%s >> 3), (%s & 7)",
     AOP(IC_RESULT(ic))->aopu.aop_dir,
     AOP(IC_RESULT(ic))->aopu.aop_dir);
  emitcode("btfsc","(%s >> 3), (%s & 7)",
     AOP(IC_RIGHT(ic))->aopu.aop_dir,
     AOP(IC_RIGHT(ic))->aopu.aop_dir);
  emitcode("xorwf","(%s >>3),f",
     AOP(IC_RESULT(ic))->aopu.aop_dir);
  emitcode("btfsc","(%s >> 3), (%s & 7)",
     AOP(IC_LEFT(ic))->aopu.aop_dir,
     AOP(IC_LEFT(ic))->aopu.aop_dir);
  emitcode("xorwf","(%s>>3),f",
     AOP(IC_RESULT(ic))->aopu.aop_dir);
    } else {

      emitcode("clrw","");
      emitcode("btfsc","(%s >> 3), (%s & 7)",
     AOP(IC_RIGHT(ic))->aopu.aop_dir,
     AOP(IC_RIGHT(ic))->aopu.aop_dir);
      emitcode("xorlw","1");
      emitcode("btfsc","(%s >> 3), (%s & 7)",
         AOP(IC_LEFT(ic))->aopu.aop_dir,
         AOP(IC_LEFT(ic))->aopu.aop_dir);
      emitcode("xorlw","1");
    }

}

#if 0
/* This is the original version of this code.
 *
 * This is being kept around for reference,
 * because I am not entirely sure I got it right...
 */
static void adjustArithmeticResult(iCode *ic)
{
    if (AOP_SIZE(IC_RESULT(ic)) == 3 &&
  AOP_SIZE(IC_LEFT(ic)) == 3   &&
  !sameRegs(AOP(IC_RESULT(ic)),AOP(IC_LEFT(ic))))
  aopPut(AOP(IC_RESULT(ic)),
         aopGet(AOP(IC_LEFT(ic)),2,FALSE,FALSE),
         2);

    if (AOP_SIZE(IC_RESULT(ic)) == 3 &&
  AOP_SIZE(IC_RIGHT(ic)) == 3   &&
  !sameRegs(AOP(IC_RESULT(ic)),AOP(IC_RIGHT(ic))))
  aopPut(AOP(IC_RESULT(ic)),
         aopGet(AOP(IC_RIGHT(ic)),2,FALSE,FALSE),
         2);

    if (AOP_SIZE(IC_RESULT(ic)) == 3 &&
  AOP_SIZE(IC_LEFT(ic)) < 3    &&
  AOP_SIZE(IC_RIGHT(ic)) < 3   &&
  !sameRegs(AOP(IC_RESULT(ic)),AOP(IC_LEFT(ic))) &&
  !sameRegs(AOP(IC_RESULT(ic)),AOP(IC_RIGHT(ic)))) {
  char buffer[5];
  sprintf(buffer,"#%d",pointerCode(getSpec(operandType(IC_LEFT(ic)))));
  aopPut(AOP(IC_RESULT(ic)),buffer,2);
    }
}
//#else
/* This is the pure and virtuous version of this code.
 * I'm pretty certain it's right, but not enough to toss the old
 * code just yet...
 */
static void adjustArithmeticResult(iCode *ic)
{
    if (opIsGptr(IC_RESULT(ic)) &&
      opIsGptr(IC_LEFT(ic))   &&
  !sameRegs(AOP(IC_RESULT(ic)),AOP(IC_LEFT(ic))))
    {
  aopPut(AOP(IC_RESULT(ic)),
         aopGet(AOP(IC_LEFT(ic)), GPTRSIZE - 1,FALSE,FALSE),
         GPTRSIZE - 1);
    }

    if (opIsGptr(IC_RESULT(ic)) &&
        opIsGptr(IC_RIGHT(ic))   &&
  !sameRegs(AOP(IC_RESULT(ic)),AOP(IC_RIGHT(ic))))
    {
  aopPut(AOP(IC_RESULT(ic)),
         aopGet(AOP(IC_RIGHT(ic)),GPTRSIZE - 1,FALSE,FALSE),
         GPTRSIZE - 1);
    }

    if (opIsGptr(IC_RESULT(ic))      &&
        AOP_SIZE(IC_LEFT(ic)) < GPTRSIZE   &&
        AOP_SIZE(IC_RIGHT(ic)) < GPTRSIZE  &&
   !sameRegs(AOP(IC_RESULT(ic)),AOP(IC_LEFT(ic))) &&
   !sameRegs(AOP(IC_RESULT(ic)),AOP(IC_RIGHT(ic)))) {
   char buffer[5];
   sprintf(buffer,"#%d",pointerCode(getSpec(operandType(IC_LEFT(ic)))));
   aopPut(AOP(IC_RESULT(ic)),buffer,GPTRSIZE - 1);
     }
}
#endif

/*-----------------------------------------------------------------*/
/* genPlus - generates code for addition                           */
/*-----------------------------------------------------------------*/
static void genPlus (iCode *ic)
{
    int size, offset = 0;

    /* special cases :- */
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    aopOp (IC_LEFT(ic),ic,FALSE);
    aopOp (IC_RIGHT(ic),ic,FALSE);
    aopOp (IC_RESULT(ic),ic,TRUE);

    /* if literal, literal on the right or
       if left requires ACC or right is already
       in ACC */

    if (AOP_TYPE(IC_LEFT(ic)) == AOP_LIT) {
        operand *t = IC_RIGHT(ic);
        IC_RIGHT(ic) = IC_LEFT(ic);
        IC_LEFT(ic) = t;
    }

    /* if both left & right are in bit space */
    if (AOP_TYPE(IC_LEFT(ic)) == AOP_CRY &&
        AOP_TYPE(IC_RIGHT(ic)) == AOP_CRY) {
        genPlusBits (ic);
        goto release ;
    }

    /* if left in bit space & right literal */
    if (AOP_TYPE(IC_LEFT(ic)) == AOP_CRY &&
        AOP_TYPE(IC_RIGHT(ic)) == AOP_LIT) {
        /* if result in bit space */
        if(AOP_TYPE(IC_RESULT(ic)) == AOP_CRY){
    if((unsigned long)floatFromVal(AOP(IC_RIGHT(ic))->aopu.aop_lit) != 0L) {
      emitcode("movlw","(1 << (%s & 7)) ;%d",AOP(IC_RESULT(ic))->aopu.aop_dir,__LINE__);
      if (!sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))) )
        emitcode("btfsc","(%s >> 3), (%s & 7)",
           AOP(IC_LEFT(ic))->aopu.aop_dir,
           AOP(IC_LEFT(ic))->aopu.aop_dir);

      emitcode("xorwf","(%s>>3),f",AOP(IC_RESULT(ic))->aopu.aop_dir);
    }
        } else {
            size = getDataSize(IC_RESULT(ic));
            while (size--) {
                MOVA(aopGet(AOP(IC_RIGHT(ic)),offset,FALSE,FALSE));
                emitcode("addc","a,#00  ;%d",__LINE__);
                aopPut(AOP(IC_RESULT(ic)),"a",offset++);
            }
        }
        goto release ;
    }

    /* if I can do an increment instead
    of add then GOOD for ME */
    if (genPlusIncr (ic) == TRUE)
        goto release;

    size = getDataSize(IC_RESULT(ic));

    if(AOP(IC_RIGHT(ic))->type == AOP_LIT) {
      /* Add a literal to something else */
      bool know_W=0;
      unsigned lit = floatFromVal(AOP(IC_RIGHT(ic))->aopu.aop_lit);
      unsigned l1=0;

      offset = 0;
      DEBUGemitcode(";","adding lit to something. size %d",size);
      while(size--){

      DEBUGemitcode(";","size %d",size);

  switch (lit & 0xff) {
  case 0:
    break;
  case 1:
    if(sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))))
      emitcode("incf","%s,f", aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));
    else {
      know_W = 0;
      emitcode("incf","%s,w", aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));
      if(AOP_TYPE(IC_RESULT(ic)) != AOP_ACC)
        emitcode("movwf","%s", aopGet(AOP(IC_RESULT(ic)),offset,FALSE,FALSE));
    }
    break;
  case 0xff:
    if(sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))))
      emitcode("decf","%s,f", aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));
    else {
      know_W = 0;
      emitcode("decf","%s,w", aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));
      if(AOP_TYPE(IC_RESULT(ic)) != AOP_ACC)
        emitcode("movwf","%s", aopGet(AOP(IC_RESULT(ic)),offset,FALSE,FALSE));
    }
    break;
  default:
    if( !know_W || ( (lit&0xff) != l1)  ) {
      know_W = 1;
      emitcode("movlw","0x%x", lit&0xff);
    }
    if(sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))))
      emitcode("addwf","%s,f", aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));
    else {
      know_W = 0;
      emitcode("addwf","%s,w", aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));
      emitcode("movwf","%s", aopGet(AOP(IC_RESULT(ic)),offset,FALSE,FALSE));
      if(size) {
        emitSKPNC;
        emitcode("incf","%s,f", aopGet(AOP(IC_LEFT(ic)),offset+1,FALSE,FALSE));
      }
    }
  }

  l1 = lit & 0xff;
  lit >>= 8;
  offset++;
      }

    } else if(AOP_TYPE(IC_RIGHT(ic)) == AOP_CRY) {

      emitcode(";bitadd","right is bit: %s",aopGet(AOP(IC_RIGHT(ic)),0,FALSE,FALSE));
      emitcode(";bitadd","left is bit: %s",aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
      emitcode(";bitadd","result is bit: %s",aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));

      /* here we are adding a bit to a char or int */
      if(size == 1) {
  if (sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))) ) {

    emitcode("btfsc","(%s >> 3), (%s & 7)",
       AOP(IC_RIGHT(ic))->aopu.aop_dir,
       AOP(IC_RIGHT(ic))->aopu.aop_dir);
    emitcode(" incf","%s,f", aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));
  } else {

    if(AOP_TYPE(IC_LEFT(ic)) == AOP_ACC) {
      emitcode("btfsc","(%s >> 3), (%s & 7)",
         AOP(IC_RIGHT(ic))->aopu.aop_dir,
         AOP(IC_RIGHT(ic))->aopu.aop_dir);
      emitcode(" xorlw","1");
    } else {
      emitcode("movf","%s,w", aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
      emitcode("btfsc","(%s >> 3), (%s & 7)",
         AOP(IC_RIGHT(ic))->aopu.aop_dir,
         AOP(IC_RIGHT(ic))->aopu.aop_dir);
      emitcode(" incf","%s,w", aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
    }

    if(AOP_TYPE(IC_RESULT(ic)) != AOP_ACC) {

      if(AOP_TYPE(IC_RESULT(ic)) == AOP_CRY) {
        emitcode("andlw","1");
        emitcode("bcf","(%s >> 3), (%s & 7)",
           AOP(IC_RESULT(ic))->aopu.aop_dir,
           AOP(IC_RESULT(ic))->aopu.aop_dir);
        emitSKPZ;
        emitcode("bsf","(%s >> 3), (%s & 7)",
           AOP(IC_RESULT(ic))->aopu.aop_dir,
           AOP(IC_RESULT(ic))->aopu.aop_dir);

      } else
    emitcode("movwf","%s", aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));
    }
  }

      } else {
  int offset = 1;

  if (sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))) ) {
    emitcode("clrz","");

    emitcode("btfsc","(%s >> 3), (%s & 7)",
       AOP(IC_RIGHT(ic))->aopu.aop_dir,
       AOP(IC_RIGHT(ic))->aopu.aop_dir);
    emitcode(" incf","%s,f", aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));

  } else {

    emitcode("movf","%s,w", aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
    emitcode("btfsc","(%s >> 3), (%s & 7)",
       AOP(IC_RIGHT(ic))->aopu.aop_dir,
       AOP(IC_RIGHT(ic))->aopu.aop_dir);
    emitcode(" incf","%s,w", aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
    emitcode("movwf","%s", aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));

  }

  while(--size){
    emitSKPZ;
    emitcode(" incf","%s,f", aopGet(AOP(IC_RIGHT(ic)),offset++,FALSE,FALSE));
  }

      }

    } else {

      if(strcmp(aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE),"a") == 0 ) {
  emitcode("addwf","%s,w", aopGet(AOP(IC_RIGHT(ic)),0,FALSE,FALSE));
  emitcode("movwf","%s", aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));
      } else {
  //if ( (AOP_TYPE(IC_LEFT(ic)) == AOP_ACC) || (AOP_TYPE(IC_RESULT(ic)) == AOP_ACC) )
  if ( AOP_TYPE(IC_LEFT(ic)) == AOP_ACC) {
    emitcode("addwf","%s,w", aopGet(AOP(IC_RIGHT(ic)),0,FALSE,FALSE));
    if ( AOP_TYPE(IC_RESULT(ic)) != AOP_ACC)
      emitcode("movwf","%s", aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));
  } else {

  emitcode("movf","%s,w", aopGet(AOP(IC_RIGHT(ic)),0,FALSE,FALSE));

    if (sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))) )
        emitcode("addwf","%s,f", aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
    else {
      if( (AOP_TYPE(IC_LEFT(ic)) == AOP_IMMD) ||
    (AOP_TYPE(IC_LEFT(ic)) == AOP_LIT) ) {
        emitcode("addlw","%s", aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
      } else {
        emitcode("addwf","%s,w", aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
        if ( AOP_TYPE(IC_RESULT(ic)) != AOP_ACC)
    emitcode("movwf","%s", aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));
      }
    }
  }
      }

      offset = 1;
      size--;

      while(size--){
  if (!sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))) ) {
    emitcode("movf","%s,w",  aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));
    emitcode("movwf","%s",  aopGet(AOP(IC_RESULT(ic)),offset,FALSE,FALSE));
  }
  emitcode("movf","%s,w",  aopGet(AOP(IC_RIGHT(ic)),offset,FALSE,FALSE));
  emitSKPNC;
  emitcode("incfsz","%s,w",aopGet(AOP(IC_RIGHT(ic)),offset,FALSE,FALSE));
  emitcode("addwf","%s,f", aopGet(AOP(IC_RESULT(ic)),offset,FALSE,FALSE));

      }

    }

    //adjustArithmeticResult(ic);

 release:
      freeAsmop(IC_LEFT(ic),NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
      freeAsmop(IC_RIGHT(ic),NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
      freeAsmop(IC_RESULT(ic),NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genMinusDec :- does subtraction with decrement if possible     */
/*-----------------------------------------------------------------*/
static bool genMinusDec (iCode *ic)
{
    unsigned int icount ;
    unsigned int size = getDataSize(IC_RESULT(ic));

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* will try to generate an increment */
    /* if the right side is not a literal
    we cannot */
    if (AOP_TYPE(IC_RIGHT(ic)) != AOP_LIT)
        return FALSE ;

    DEBUGemitcode ("; lit val","%d",(unsigned int) floatFromVal (AOP(IC_RIGHT(ic))->aopu.aop_lit));

    /* if the literal value of the right hand side
    is greater than 4 then it is not worth it */
    if ((icount = (unsigned int) floatFromVal (AOP(IC_RIGHT(ic))->aopu.aop_lit)) > 2)
        return FALSE ;

    /* if decrement 16 bits in register */
    if (sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic))) &&
        (size > 1) &&
        (icount == 1)) {

      if(size == 2) {
  emitcode("decf","%s,f",aopGet(AOP(IC_RESULT(ic)),LSB,FALSE,FALSE));
  emitcode("incfsz","%s,w",aopGet(AOP(IC_RESULT(ic)),LSB,FALSE,FALSE));
  emitcode(" decf","%s,f",aopGet(AOP(IC_RESULT(ic)),MSB16,FALSE,FALSE));
      } else {
  /* size is 3 or 4 */
  emitcode("movlw","0xff");
  emitcode("addwf","%s,f",aopGet(AOP(IC_RESULT(ic)),LSB,FALSE,FALSE));

  emitSKPNC;
  emitcode("addwf","%s,f",aopGet(AOP(IC_RESULT(ic)),MSB16,FALSE,FALSE));
  emitSKPNC;
  emitcode("addwf","%s,f",aopGet(AOP(IC_RESULT(ic)),MSB24,FALSE,FALSE));

  if(size > 3) {
    emitcode("skpnc","");
    emitSKPNC;
    emitcode("addwf","%s,f",aopGet(AOP(IC_RESULT(ic)),MSB32,FALSE,FALSE));
  }

      }

      return TRUE;

    }

    /* if the sizes are greater than 1 then we cannot */
    if (AOP_SIZE(IC_RESULT(ic)) > 1 ||
        AOP_SIZE(IC_LEFT(ic)) > 1   )
        return FALSE ;

    /* we can if the aops of the left & result match or
    if they are in registers and the registers are the
    same */
    if (sameRegs(AOP(IC_LEFT(ic)), AOP(IC_RESULT(ic)))) {

        while (icount--)
            emitcode ("decf","%s,f",aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));

        return TRUE ;
    }

    DEBUGemitcode ("; returning"," result=%s, left=%s",
       aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE),
       aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
    if(size==1) {
      emitcode("decf","%s,w",aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));
      emitcode("movwf","%s",aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));
      return TRUE;
    }

    return FALSE ;
}

/*-----------------------------------------------------------------*/
/* addSign - complete with sign                                    */
/*-----------------------------------------------------------------*/
static void addSign(operand *result, int offset, int sign)
{
    int size = (getDataSize(result) - offset);
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(size > 0){
        if(sign){
            emitcode("rlc","a");
            emitcode("subb","a,acc");
            while(size--)
                aopPut(AOP(result),"a",offset++);
        } else
            while(size--)
                aopPut(AOP(result),zero,offset++);
    }
}

/*-----------------------------------------------------------------*/
/* genMinusBits - generates code for subtraction  of two bits      */
/*-----------------------------------------------------------------*/
static void genMinusBits (iCode *ic)
{
    symbol *lbl = newiTempLabel(NULL);
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if (AOP_TYPE(IC_RESULT(ic)) == AOP_CRY){
        emitcode("mov","c,%s",AOP(IC_LEFT(ic))->aopu.aop_dir);
        emitcode("jnb","%s,%05d_DS_",AOP(IC_RIGHT(ic))->aopu.aop_dir,(lbl->key+100));
        emitcode("cpl","c");
        emitcode("","%05d_DS_:",(lbl->key+100));
        outBitC(IC_RESULT(ic));
    }
    else{
        emitcode("mov","c,%s",AOP(IC_RIGHT(ic))->aopu.aop_dir);
        emitcode("subb","a,acc");
        emitcode("jnb","%s,%05d_DS_",AOP(IC_LEFT(ic))->aopu.aop_dir,(lbl->key+100));
        emitcode("inc","a");
        emitcode("","%05d_DS_:",(lbl->key+100));
        aopPut(AOP(IC_RESULT(ic)),"a",0);
        addSign(IC_RESULT(ic), MSB16, SPEC_USIGN(getSpec(operandType(IC_RESULT(ic)))));
    }
}

/*-----------------------------------------------------------------*/
/* genMinus - generates code for subtraction                       */
/*-----------------------------------------------------------------*/
static void genMinus (iCode *ic)
{
    int size, offset = 0;
    unsigned long lit = 0L;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    aopOp (IC_LEFT(ic),ic,FALSE);
    aopOp (IC_RIGHT(ic),ic,FALSE);
    aopOp (IC_RESULT(ic),ic,TRUE);

    /* special cases :- */
    /* if both left & right are in bit space */
    if (AOP_TYPE(IC_LEFT(ic)) == AOP_CRY &&
        AOP_TYPE(IC_RIGHT(ic)) == AOP_CRY) {
        genMinusBits (ic);
        goto release ;
    }

    /* if I can do an decrement instead
    of subtract then GOOD for ME */
    if (genMinusDec (ic) == TRUE)
        goto release;

    size = getDataSize(IC_RESULT(ic));

    if(AOP(IC_RIGHT(ic))->type == AOP_LIT) {
      /* Add a literal to something else */

      lit = (unsigned long)floatFromVal(AOP(IC_RIGHT(ic))->aopu.aop_lit);
      lit = - (long)lit;

      /* add the first byte: */
      emitcode("movlw","0x%x", lit & 0xff);
      emitcode("addwf","%s,f", aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));

      offset = 1;
      size--;

      while(size--){

  emitcode("rlf","_known_zero,w");
  emitcode("addwf","%s,f", aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));

  lit >>= 8;
  if(lit & 0xff) {
    emitcode("movlw","0x%x", lit & 0xff);
    emitcode("addwf","%s,f", aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));

  }
  offset++;
      }

    } else {

      emitcode("movf","%s", aopGet(AOP(IC_RIGHT(ic)),0,FALSE,FALSE));
      emitcode("subwf","%s,f", aopGet(AOP(IC_LEFT(ic)),0,FALSE,FALSE));

      offset = 1;
      size--;

      while(size--){
  emitcode("movf","%s,w",  aopGet(AOP(IC_RIGHT(ic)),offset,FALSE,FALSE));
  emitSKPNC;
  emitcode("incfsz","%s,w",aopGet(AOP(IC_RIGHT(ic)),offset,FALSE,FALSE));
  emitcode("subwf","%s,f", aopGet(AOP(IC_LEFT(ic)),offset,FALSE,FALSE));

      }

    }


    //    adjustArithmeticResult(ic);

release:
    freeAsmop(IC_LEFT(ic),NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(IC_RIGHT(ic),NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(IC_RESULT(ic),NULL,ic,TRUE);
}


/*-----------------------------------------------------------------*/
/* genMultbits :- multiplication of bits                           */
/*-----------------------------------------------------------------*/
static void genMultbits (operand *left,
                         operand *right,
                         operand *result)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
    emitcode("anl","c,%s",AOP(right)->aopu.aop_dir);
    outBitC(result);
}


/*-----------------------------------------------------------------*/
/* genMultOneByte : 8 bit multiplication & division                */
/*-----------------------------------------------------------------*/
static void genMultOneByte (operand *left,
                            operand *right,
                            operand *result)
{
    sym_link *opetype = operandType(result);
    char *l ;
    symbol *lbl ;
    int size,offset;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* (if two literals, the value is computed before) */
    /* if one literal, literal on the right */
    if (AOP_TYPE(left) == AOP_LIT){
        operand *t = right;
        right = left;
        left = t;
    }

    size = AOP_SIZE(result);
    /* signed or unsigned */
    emitcode("mov","b,%s", aopGet(AOP(right),0,FALSE,FALSE));
    l = aopGet(AOP(left),0,FALSE,FALSE);
    MOVA(l);
    emitcode("mul","ab");
    /* if result size = 1, mul signed = mul unsigned */
    aopPut(AOP(result),"a",0);
    if (size > 1){
        if (SPEC_USIGN(opetype)){
            aopPut(AOP(result),"b",1);
            if (size > 2)
                /* for filling the MSBs */
                emitcode("clr","a");
        }
        else{
            emitcode("mov","a,b");

            /* adjust the MSB if left or right neg */

            /* if one literal */
            if (AOP_TYPE(right) == AOP_LIT){
                /* AND literal negative */
                if((int) floatFromVal (AOP(right)->aopu.aop_lit) < 0){
                    /* adjust MSB (c==0 after mul) */
                    emitcode("subb","a,%s", aopGet(AOP(left),0,FALSE,FALSE));
                }
            }
            else{
                lbl = newiTempLabel(NULL);
                emitcode("xch","a,%s",aopGet(AOP(right),0,FALSE,FALSE));
                emitcode("cjne","a,#0x80,%05d_DS_", (lbl->key+100));
                emitcode("","%05d_DS_:",(lbl->key+100));
                emitcode("xch","a,%s",aopGet(AOP(right),0,FALSE,FALSE));
                lbl = newiTempLabel(NULL);
                emitcode("jc","%05d_DS_",(lbl->key+100));
                emitcode("subb","a,%s", aopGet(AOP(left),0,FALSE,FALSE));
                emitcode("","%05d_DS_:",(lbl->key+100));
            }

            lbl = newiTempLabel(NULL);
            emitcode("xch","a,%s",aopGet(AOP(left),0,FALSE,FALSE));
            emitcode("cjne","a,#0x80,%05d_DS_", (lbl->key+100));
            emitcode("","%05d_DS_:",(lbl->key+100));
            emitcode("xch","a,%s",aopGet(AOP(left),0,FALSE,FALSE));
            lbl = newiTempLabel(NULL);
            emitcode("jc","%05d_DS_",(lbl->key+100));
            emitcode("subb","a,%s", aopGet(AOP(right),0,FALSE,FALSE));
            emitcode("","%05d_DS_:",(lbl->key+100));

            aopPut(AOP(result),"a",1);
            if(size > 2){
                /* get the sign */
                emitcode("rlc","a");
                emitcode("subb","a,acc");
            }
        }
        size -= 2;
        offset = 2;
        if (size > 0)
            while (size--)
                aopPut(AOP(result),"a",offset++);
    }
}

/*-----------------------------------------------------------------*/
/* genMult - generates code for multiplication                     */
/*-----------------------------------------------------------------*/
static void genMult (iCode *ic)
{
    operand *left = IC_LEFT(ic);
    operand *right = IC_RIGHT(ic);
    operand *result= IC_RESULT(ic);

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* assign the amsops */
    aopOp (left,ic,FALSE);
    aopOp (right,ic,FALSE);
    aopOp (result,ic,TRUE);

    /* special cases first */
    /* both are bits */
    if (AOP_TYPE(left) == AOP_CRY &&
        AOP_TYPE(right)== AOP_CRY) {
        genMultbits(left,right,result);
        goto release ;
    }

    /* if both are of size == 1 */
    if (AOP_SIZE(left) == 1 &&
        AOP_SIZE(right) == 1 ) {
        genMultOneByte(left,right,result);
        goto release ;
    }

    /* should have been converted to function call */
    assert(1) ;

release :
    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genDivbits :- division of bits                                  */
/*-----------------------------------------------------------------*/
static void genDivbits (operand *left,
                        operand *right,
                        operand *result)
{

    char *l;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* the result must be bit */
    emitcode("mov","b,%s",aopGet(AOP(right),0,FALSE,FALSE));
    l = aopGet(AOP(left),0,FALSE,FALSE);

    MOVA(l);

    emitcode("div","ab");
    emitcode("rrc","a");
    aopPut(AOP(result),"c",0);
}

/*-----------------------------------------------------------------*/
/* genDivOneByte : 8 bit division                                  */
/*-----------------------------------------------------------------*/
static void genDivOneByte (operand *left,
                           operand *right,
                           operand *result)
{
    sym_link *opetype = operandType(result);
    char *l ;
    symbol *lbl ;
    int size,offset;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    size = AOP_SIZE(result) - 1;
    offset = 1;
    /* signed or unsigned */
    if (SPEC_USIGN(opetype)) {
        /* unsigned is easy */
        emitcode("mov","b,%s", aopGet(AOP(right),0,FALSE,FALSE));
        l = aopGet(AOP(left),0,FALSE,FALSE);
        MOVA(l);
        emitcode("div","ab");
        aopPut(AOP(result),"a",0);
        while (size--)
            aopPut(AOP(result),zero,offset++);
        return ;
    }

    /* signed is a little bit more difficult */

    /* save the signs of the operands */
    l = aopGet(AOP(left),0,FALSE,FALSE);
    MOVA(l);
    emitcode("xrl","a,%s",aopGet(AOP(right),0,FALSE,TRUE));
    emitcode("push","acc"); /* save it on the stack */

    /* now sign adjust for both left & right */
    l =  aopGet(AOP(right),0,FALSE,FALSE);
    MOVA(l);
    lbl = newiTempLabel(NULL);
    emitcode("jnb","acc.7,%05d_DS_",(lbl->key+100));
    emitcode("cpl","a");
    emitcode("inc","a");
    emitcode("","%05d_DS_:",(lbl->key+100));
    emitcode("mov","b,a");

    /* sign adjust left side */
    l =  aopGet(AOP(left),0,FALSE,FALSE);
    MOVA(l);

    lbl = newiTempLabel(NULL);
    emitcode("jnb","acc.7,%05d_DS_",(lbl->key+100));
    emitcode("cpl","a");
    emitcode("inc","a");
    emitcode("","%05d_DS_:",(lbl->key+100));

    /* now the division */
    emitcode("div","ab");
    /* we are interested in the lower order
    only */
    emitcode("mov","b,a");
    lbl = newiTempLabel(NULL);
    emitcode("pop","acc");
    /* if there was an over flow we don't
    adjust the sign of the result */
    emitcode("jb","ov,%05d_DS_",(lbl->key+100));
    emitcode("jnb","acc.7,%05d_DS_",(lbl->key+100));
    CLRC;
    emitcode("clr","a");
    emitcode("subb","a,b");
    emitcode("mov","b,a");
    emitcode("","%05d_DS_:",(lbl->key+100));

    /* now we are done */
    aopPut(AOP(result),"b",0);
    if(size > 0){
        emitcode("mov","c,b.7");
        emitcode("subb","a,acc");
    }
    while (size--)
        aopPut(AOP(result),"a",offset++);

}

/*-----------------------------------------------------------------*/
/* genDiv - generates code for division                            */
/*-----------------------------------------------------------------*/
static void genDiv (iCode *ic)
{
    operand *left = IC_LEFT(ic);
    operand *right = IC_RIGHT(ic);
    operand *result= IC_RESULT(ic);

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* assign the amsops */
    aopOp (left,ic,FALSE);
    aopOp (right,ic,FALSE);
    aopOp (result,ic,TRUE);

    /* special cases first */
    /* both are bits */
    if (AOP_TYPE(left) == AOP_CRY &&
        AOP_TYPE(right)== AOP_CRY) {
        genDivbits(left,right,result);
        goto release ;
    }

    /* if both are of size == 1 */
    if (AOP_SIZE(left) == 1 &&
        AOP_SIZE(right) == 1 ) {
        genDivOneByte(left,right,result);
        goto release ;
    }

    /* should have been converted to function call */
    assert(1);
release :
    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genModbits :- modulus of bits                                   */
/*-----------------------------------------------------------------*/
static void genModbits (operand *left,
                        operand *right,
                        operand *result)
{

    char *l;

    /* the result must be bit */
    emitcode("mov","b,%s",aopGet(AOP(right),0,FALSE,FALSE));
    l = aopGet(AOP(left),0,FALSE,FALSE);

    MOVA(l);

    emitcode("div","ab");
    emitcode("mov","a,b");
    emitcode("rrc","a");
    aopPut(AOP(result),"c",0);
}

/*-----------------------------------------------------------------*/
/* genModOneByte : 8 bit modulus                                   */
/*-----------------------------------------------------------------*/
static void genModOneByte (operand *left,
                           operand *right,
                           operand *result)
{
    sym_link *opetype = operandType(result);
    char *l ;
    symbol *lbl ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* signed or unsigned */
    if (SPEC_USIGN(opetype)) {
        /* unsigned is easy */
        emitcode("mov","b,%s", aopGet(AOP(right),0,FALSE,FALSE));
        l = aopGet(AOP(left),0,FALSE,FALSE);
        MOVA(l);
        emitcode("div","ab");
        aopPut(AOP(result),"b",0);
        return ;
    }

    /* signed is a little bit more difficult */

    /* save the signs of the operands */
    l = aopGet(AOP(left),0,FALSE,FALSE);
    MOVA(l);

    emitcode("xrl","a,%s",aopGet(AOP(right),0,FALSE,FALSE));
    emitcode("push","acc"); /* save it on the stack */

    /* now sign adjust for both left & right */
    l =  aopGet(AOP(right),0,FALSE,FALSE);
    MOVA(l);

    lbl = newiTempLabel(NULL);
    emitcode("jnb","acc.7,%05d_DS_",(lbl->key+100));
    emitcode("cpl","a");
    emitcode("inc","a");
    emitcode("","%05d_DS_:",(lbl->key+100));
    emitcode("mov","b,a");

    /* sign adjust left side */
    l =  aopGet(AOP(left),0,FALSE,FALSE);
    MOVA(l);

    lbl = newiTempLabel(NULL);
    emitcode("jnb","acc.7,%05d_DS_",(lbl->key+100));
    emitcode("cpl","a");
    emitcode("inc","a");
    emitcode("","%05d_DS_:",(lbl->key+100));

    /* now the multiplication */
    emitcode("div","ab");
    /* we are interested in the lower order
    only */
    lbl = newiTempLabel(NULL);
    emitcode("pop","acc");
    /* if there was an over flow we don't
    adjust the sign of the result */
    emitcode("jb","ov,%05d_DS_",(lbl->key+100));
    emitcode("jnb","acc.7,%05d_DS_",(lbl->key+100));
    CLRC ;
    emitcode("clr","a");
    emitcode("subb","a,b");
    emitcode("mov","b,a");
    emitcode("","%05d_DS_:",(lbl->key+100));

    /* now we are done */
    aopPut(AOP(result),"b",0);

}

/*-----------------------------------------------------------------*/
/* genMod - generates code for division                            */
/*-----------------------------------------------------------------*/
static void genMod (iCode *ic)
{
    operand *left = IC_LEFT(ic);
    operand *right = IC_RIGHT(ic);
    operand *result= IC_RESULT(ic);

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* assign the amsops */
    aopOp (left,ic,FALSE);
    aopOp (right,ic,FALSE);
    aopOp (result,ic,TRUE);

    /* special cases first */
    /* both are bits */
    if (AOP_TYPE(left) == AOP_CRY &&
        AOP_TYPE(right)== AOP_CRY) {
        genModbits(left,right,result);
        goto release ;
    }

    /* if both are of size == 1 */
    if (AOP_SIZE(left) == 1 &&
        AOP_SIZE(right) == 1 ) {
        genModOneByte(left,right,result);
        goto release ;
    }

    /* should have been converted to function call */
    assert(1);

release :
    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genIfxJump :- will create a jump depending on the ifx           */
/*-----------------------------------------------------------------*/
static void genIfxJump (iCode *ic, char *jval)
{

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if true label then we jump if condition
    supplied is true */
    if ( IC_TRUE(ic) ) {

  if(strcmp(jval,"a") == 0)
    emitSKPZ;
  else if (strcmp(jval,"c") == 0)
    emitSKPC;
  else
    emitcode("btfsc","(%s >> 3),(%s & 7)",jval,jval);

  emitcode(" goto","_%05d_DS_",IC_TRUE(ic)->key+100 + labelOffset);

    }
    else {
        /* false label is present */
  if(strcmp(jval,"a") == 0)
    emitSKPNZ;
  else if (strcmp(jval,"c") == 0)
    emitSKPNC;
  else
    emitcode("btfss","(%s >> 3),(%s & 7)",jval,jval);

  emitcode(" goto","_%05d_DS_",IC_FALSE(ic)->key+100 + labelOffset);

    }


    /* mark the icode as generated */
    ic->generated = 1;
}

/*-----------------------------------------------------------------*/
/* genSkip                                                         */
/*-----------------------------------------------------------------*/
static void genSkip(iCode *ifx,int status_bit)
{
  if(!ifx)
    return;

  if ( IC_TRUE(ifx) ) {
    switch(status_bit) {
    case 'z':
      emitSKPNZ;
      break;

    case 'c':
      emitSKPNC;
      break;

    case 'd':
      emitcode("skpndc","");
      break;

    }

    emitcode("goto","_%05d_DS_",IC_TRUE(ifx)->key+100+labelOffset);

  } else {

    switch(status_bit) {

    case 'z':
      emitSKPZ;
      break;

    case 'c':
      emitSKPC;
      break;

    case 'd':
      emitcode("skpdc","");
      break;
    }
    emitcode("goto","_%05d_DS_",IC_FALSE(ifx)->key+100+labelOffset);

  }

}

/*-----------------------------------------------------------------*/
/* genSkipc                                                        */
/*-----------------------------------------------------------------*/
static void genSkipc(iCode *ifx, int condition)
{
  if(!ifx)
    return;

  if(condition)
    emitSKPNC;
  else
    emitSKPC;

  if ( IC_TRUE(ifx) )
    emitcode("goto","_%05d_DS_",IC_TRUE(ifx)->key+100+labelOffset);
  else
    emitcode("goto","_%05d_DS_",IC_FALSE(ifx)->key+100+labelOffset);

}

/*-----------------------------------------------------------------*/
/* genSkipz                                                        */
/*-----------------------------------------------------------------*/
static void genSkipz(iCode *ifx, int condition)
{
  if(!ifx)
    return;

  if(condition)
    emitSKPNZ;
  else
    emitSKPZ;

  if ( IC_TRUE(ifx) )
    emitcode("goto","_%05d_DS_",IC_TRUE(ifx)->key+100+labelOffset);
  else
    emitcode("goto","_%05d_DS_",IC_FALSE(ifx)->key+100+labelOffset);

}
/*-----------------------------------------------------------------*/
/* genCmp :- greater or less than comparison                       */
/*-----------------------------------------------------------------*/
static void genCmp (operand *left,operand *right,
                    operand *result, iCode *ifx, int sign)
{
  int size, offset = 0 ;
  unsigned long lit = 0L,i = 0;

  DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
  /* if left & right are bit variables */
  if (AOP_TYPE(left) == AOP_CRY &&
      AOP_TYPE(right) == AOP_CRY ) {
    emitcode("mov","c,%s",AOP(right)->aopu.aop_dir);
    emitcode("anl","c,/%s",AOP(left)->aopu.aop_dir);
  } else {
    /* subtract right from left if at the
       end the carry flag is set then we know that
       left is greater than right */
    size = max(AOP_SIZE(left),AOP_SIZE(right));

    /* if unsigned char cmp with lit, do cjne left,#right,zz */
    if((size == 1) && !sign &&
       (AOP_TYPE(right) == AOP_LIT && AOP_TYPE(left) != AOP_DIR )){
      symbol *lbl  = newiTempLabel(NULL);
      emitcode("cjne","%s,%s,%05d_DS_",
         aopGet(AOP(left),offset,FALSE,FALSE),
         aopGet(AOP(right),offset,FALSE,FALSE),
         lbl->key+100);
      emitcode("","%05d_DS_:",lbl->key+100);
    } else {

      if(AOP_TYPE(right) == AOP_LIT) {

  DEBUGemitcode(";right lit","%d",sign);

  lit = (unsigned long)floatFromVal(AOP(right)->aopu.aop_lit);
  //default:
  while(size--) {
    i = (lit >> (size*8)) & 0xff;
    if(i == 0) {
      emitcode("movf","%s,w",aopGet(AOP(left),size,FALSE,FALSE));
      genSkipz(ifx,IC_TRUE(ifx) == NULL);
    } else {
      emitcode("movlw","0x%x",i);
      emitcode("subwf","%s,w",aopGet(AOP(left),size,FALSE,FALSE));
      genSkipc(ifx,IC_TRUE(ifx) == NULL);
    }

  }
  ifx->generated = 1;
  return;
      }
      if(AOP_TYPE(left) == AOP_LIT) {

  DEBUGemitcode(";left lit","%d",sign);

  lit = (unsigned long)(floatFromVal(AOP(left)->aopu.aop_lit))+1;

  //default:
  while(size--) {
    i = (lit >> (size*8)) & 0xff;
    if(i == 0) {
      emitcode("movf","%s,w",aopGet(AOP(right),size,FALSE,FALSE));
      genSkipz(ifx,IC_TRUE(ifx) != NULL);
    } else if( i == 1 ) {
      emitcode("decf","%s,w",aopGet(AOP(right),size,FALSE,FALSE));
      genSkipz(ifx,IC_TRUE(ifx) != NULL);

    } else {
      emitcode("movlw","0x%x",i);
      emitcode("subwf","%s,w",aopGet(AOP(right),size,FALSE,FALSE));
      genSkipc(ifx,IC_TRUE(ifx) != NULL);
    }
  }
  ifx->generated = 1;
  return;
      }


      // CLRC;
      DEBUGemitcode(";sign","%d",sign);
      emitcode("movf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
      emitcode("subwf","%s,w",aopGet(AOP(left),offset++,FALSE,FALSE));
      size--;
      while (size--) {

  /*if (AOP_TYPE(right) == AOP_LIT){
    unsigned long lit = (unsigned long)
    floatFromVal(AOP(right)->aopu.aop_lit);
    emitcode("subb","a,#0x%02x",
    0x80 ^ (unsigned int)((lit >> (offset*8)) & 0x0FFL));
    } else {
    emitcode("mov","b,%s",aopGet(AOP(right),offset++,FALSE,FALSE));
    emitcode("xrl","b,#0x80");
    emitcode("subb","a,b");
    }
    } else
    emitcode("subb","a,%s",aopGet(AOP(right),offset++,FALSE,FALSE));
  */
  emitcode("movf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
  emitSKPC;
  emitcode("incfsz","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
  emitcode("subwf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
  offset++;
      }
    }
  }

  //release:
  if (AOP_TYPE(result) == AOP_CRY && AOP_SIZE(result)) {
    outBitC(result);
  } else {
    /* if the result is used in the next
       ifx conditional branch then generate
       code a little differently */
    if (ifx )
      genIfxJump (ifx,"c");
    else
      outBitC(result);
    /* leave the result in acc */
  }

}

/*-----------------------------------------------------------------*/
/* genCmpGt :- greater than comparison                             */
/*-----------------------------------------------------------------*/
static void genCmpGt (iCode *ic, iCode *ifx)
{
    operand *left, *right, *result;
    sym_link *letype , *retype;
    int sign ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    left = IC_LEFT(ic);
    right= IC_RIGHT(ic);
    result = IC_RESULT(ic);

    letype = getSpec(operandType(left));
    retype =getSpec(operandType(right));
    sign =  !(SPEC_USIGN(letype) | SPEC_USIGN(retype));
    /* assign the amsops */
    aopOp (left,ic,FALSE);
    aopOp (right,ic,FALSE);
    aopOp (result,ic,TRUE);

    genCmp(right, left, result, ifx, sign);

    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genCmpLt - less than comparisons                                */
/*-----------------------------------------------------------------*/
static void genCmpLt (iCode *ic, iCode *ifx)
{
    operand *left, *right, *result;
    sym_link *letype , *retype;
    int sign ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    left = IC_LEFT(ic);
    right= IC_RIGHT(ic);
    result = IC_RESULT(ic);

    letype = getSpec(operandType(left));
    retype =getSpec(operandType(right));
    sign =  !(SPEC_USIGN(letype) | SPEC_USIGN(retype));

    /* assign the amsops */
    aopOp (left,ic,FALSE);
    aopOp (right,ic,FALSE);
    aopOp (result,ic,TRUE);

    genCmp(left, right, result, ifx, sign);

    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* gencjneshort - compare and jump if not equal                    */
/*-----------------------------------------------------------------*/
static void gencjneshort(operand *left, operand *right, symbol *lbl)
{
    int size = max(AOP_SIZE(left),AOP_SIZE(right));
    int offset = 0;
    unsigned long lit = 0L;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if the left side is a literal or
    if the right is in a pointer register and left
    is not */
    if ((AOP_TYPE(left) == AOP_LIT) ||
        (IS_AOP_PREG(right) && !IS_AOP_PREG(left))) {
        operand *t = right;
        right = left;
        left = t;
    }
    if(AOP_TYPE(right) == AOP_LIT)
        lit = (unsigned long)floatFromVal(AOP(right)->aopu.aop_lit);

    /* if the right side is a literal then anything goes */
    if (AOP_TYPE(right) == AOP_LIT &&
        AOP_TYPE(left) != AOP_DIR ) {
        while (size--) {
    if(lit & 0xff) {
      emitcode("movf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      emitcode("xorlw","0x%x",lit & 0xff);
    } else
      emitcode("movf","%s,f",aopGet(AOP(left),offset,FALSE,FALSE));

    emitSKPNZ;
    emitcode("goto","_%05d_DS_",lbl->key+100+labelOffset);
    offset++;
    lit >>= 8;
        }
    }

    /* if the right side is in a register or in direct space or
    if the left is a pointer register & right is not */
    else if (AOP_TYPE(right) == AOP_REG ||
             AOP_TYPE(right) == AOP_DIR ||
             (AOP_TYPE(left) == AOP_DIR && AOP_TYPE(right) == AOP_LIT) ||
             (IS_AOP_PREG(left) && !IS_AOP_PREG(right))) {
        while (size--) {
    if((AOP_TYPE(left) == AOP_DIR && AOP_TYPE(right) == AOP_LIT) &&
       ( (lit & 0xff) != 0)) {
      emitcode("movf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      emitcode("xorlw","0x%x",lit & 0xff);
      lit >>= 8;
    } else
      emitcode("movf","%s,f",aopGet(AOP(left),offset,FALSE,FALSE));

    emitSKPZ;
    emitcode("goto","_%05d_DS_",lbl->key+100+labelOffset);
    offset++;
/*
            MOVA(aopGet(AOP(left),offset,FALSE,FALSE));
            if((AOP_TYPE(left) == AOP_DIR && AOP_TYPE(right) == AOP_LIT) &&
               ((unsigned int)((lit >> (offset*8)) & 0x0FFL) == 0))
                emitcode("jnz","%05d_DS_",lbl->key+100);
            else
                emitcode("cjne","a,%s,%05d_DS_",
                         aopGet(AOP(right),offset,FALSE,TRUE),
                         lbl->key+100);
            offset++;
*/
        }
    } else {
        /* right is a pointer reg need both a & b */
        while(size--) {
            char *l = aopGet(AOP(left),offset,FALSE,FALSE);
            if(strcmp(l,"b"))
                emitcode("mov","b,%s",l);
            MOVA(aopGet(AOP(right),offset,FALSE,FALSE));
            emitcode("cjne","a,b,%05d_DS_",lbl->key+100);
            offset++;
        }
    }
}

/*-----------------------------------------------------------------*/
/* gencjne - compare and jump if not equal                         */
/*-----------------------------------------------------------------*/
static void gencjne(operand *left, operand *right, symbol *lbl)
{
    symbol *tlbl  = newiTempLabel(NULL);

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    gencjneshort(left, right, lbl);

    emitcode("mov","a,%s",one);
    emitcode("sjmp","%05d_DS_",tlbl->key+100);
    emitcode("","%05d_DS_:",lbl->key+100);
    emitcode("clr","a");
    emitcode("","%05d_DS_:",tlbl->key+100);
}


/*-----------------------------------------------------------------*/
/* genCmpEq - generates code for equal to                          */
/*-----------------------------------------------------------------*/
static void genCmpEq (iCode *ic, iCode *ifx)
{
    operand *left, *right, *result;
    unsigned long lit = 0L;
    int size,offset=0;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(ifx)
      DEBUGemitcode ("; ifx is non-null","");
    else
      DEBUGemitcode ("; ifx is null","");

    aopOp((left=IC_LEFT(ic)),ic,FALSE);
    aopOp((right=IC_RIGHT(ic)),ic,FALSE);
    aopOp((result=IC_RESULT(ic)),ic,TRUE);

    size = max(AOP_SIZE(left),AOP_SIZE(right));

    /* if literal, literal on the right or
    if the right is in a pointer register and left
    is not */
    if ((AOP_TYPE(IC_LEFT(ic)) == AOP_LIT) ||
        (IS_AOP_PREG(right) && !IS_AOP_PREG(left))) {
        operand *t = IC_RIGHT(ic);
        IC_RIGHT(ic) = IC_LEFT(ic);
        IC_LEFT(ic) = t;
    }

    if(ifx && !AOP_SIZE(result)){
        symbol *tlbl;
        /* if they are both bit variables */
        if (AOP_TYPE(left) == AOP_CRY &&
            ((AOP_TYPE(right) == AOP_CRY) || (AOP_TYPE(right) == AOP_LIT))) {
            if(AOP_TYPE(right) == AOP_LIT){
                unsigned long lit = (unsigned long)floatFromVal(AOP(IC_RIGHT(ic))->aopu.aop_lit);
                if(lit == 0L){
                    emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
                    emitcode("cpl","c");
                } else if(lit == 1L) {
                    emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
                } else {
                    emitcode("clr","c");
                }
                /* AOP_TYPE(right) == AOP_CRY */
            } else {
                symbol *lbl = newiTempLabel(NULL);
                emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
                emitcode("jb","%s,%05d_DS_",AOP(right)->aopu.aop_dir,(lbl->key+100));
                emitcode("cpl","c");
                emitcode("","%05d_DS_:",(lbl->key+100));
            }
            /* if true label then we jump if condition
            supplied is true */
            tlbl = newiTempLabel(NULL);
            if ( IC_TRUE(ifx) ) {
                emitcode("jnc","%05d_DS_",tlbl->key+100);
                emitcode("ljmp","%05d_DS_",IC_TRUE(ifx)->key+100);
            } else {
                emitcode("jc","%05d_DS_",tlbl->key+100);
                emitcode("ljmp","%05d_DS_",IC_FALSE(ifx)->key+100);
            }
            emitcode("","%05d_DS_:",tlbl->key+100+labelOffset);
        } else {

    /* They're not both bit variables. Is the right a literal? */
    if(AOP_TYPE(right) == AOP_LIT) {

      lit = (unsigned long)floatFromVal(AOP(right)->aopu.aop_lit);
      while (size--) {

        if(size >= 1) {
    int l = lit & 0xff;
    int h = (lit>>8) & 0xff;
    int optimized=0;

    /* Check special cases for integers */
    switch(lit & 0xffff) {
    case 0x0000:
      emitcode("movf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      emitcode("iorwf","%s,w",aopGet(AOP(left),offset+1,FALSE,FALSE));
      genSkip(ifx,'z');
      optimized++;
      break;
    case 0x0001:
      emitcode("decf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      emitcode("iorwf","%s,w",aopGet(AOP(left),offset+1,FALSE,FALSE));
      genSkip(ifx,'z');
      optimized++;
      break;
    case 0x0100:
      emitcode("decf","%s,w",aopGet(AOP(left),offset+1,FALSE,FALSE));
      emitcode("iorwf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      genSkip(ifx,'z');
      optimized++;
      break;
    case 0x00ff:
      emitcode("incf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      emitcode("iorwf","%s,w",aopGet(AOP(left),offset+1,FALSE,FALSE));
      genSkip(ifx,'z');
      optimized++;
      break;
    case 0xff00:
      emitcode("incf","%s,w",aopGet(AOP(left),offset+1,FALSE,FALSE));
      emitcode("iorwf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      genSkip(ifx,'z');
      optimized++;
      break;
    default:
      if(h == 0) {
        emitcode("movf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
        emitcode("xorlw","0x%x",l);
        emitcode("iorwf","%s,w",aopGet(AOP(left),offset+1,FALSE,FALSE));
        optimized++;
        genSkip(ifx,'z');
      } else if (l == 0) {
        emitcode("movf","%s,w",aopGet(AOP(left),offset+1,FALSE,FALSE));
        emitcode("xorlw","0x%x",h);
        emitcode("iorwf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
        optimized++;
        genSkip(ifx,'z');
      } else {
        emitcode("movf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
        emitcode("xorlw","0x%x",l);
        emitcode("movlw","0x%x",h);
        emitSKPZ;
        emitcode("xorwf","%s,w",aopGet(AOP(left),offset+1,FALSE,FALSE));
        optimized++;
        genSkip(ifx,'z');
      }

    }
    if(optimized) {
      size--;
      offset+=2;
      lit>>=16;

      continue;
    }

        }

        switch(lit & 0xff) {
        case 1:
    if ( IC_TRUE(ifx) ) {
      emitcode("decf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      emitSKPNZ;
      emitcode("goto","_%05d_DS_",IC_TRUE(ifx)->key+100+labelOffset);
    } else {
      emitcode("decfsz","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      emitcode("goto","_%05d_DS_",IC_FALSE(ifx)->key+100+labelOffset);
    }
    break;
        case 0xff:
    if ( IC_TRUE(ifx) ) {
      emitcode("incf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      emitSKPNZ;
      emitcode("goto","_%05d_DS_",IC_TRUE(ifx)->key+100+labelOffset);
    } else {
      emitcode("incfsz","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      emitcode("goto","_%05d_DS_",IC_FALSE(ifx)->key+100+labelOffset);
    }
    break;
        default:
    emitcode("movf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
    if(lit)
      emitcode("xorlw","0x%x",lit & 0xff);
    genSkip(ifx,'z');
        }


        //        emitcode("goto","_%05d_DS_",tlbl->key+100+labelOffset);
        //emitcode("","_%05d_DS_:",tlbl->key+100+labelOffset);
        offset++;
        lit >>= 8;
      }

    } else if(AOP_TYPE(right) == AOP_CRY ) {
      /* we know the left is not a bit, but that the right is */
      emitcode("movf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      if ( IC_TRUE(ifx) )
        emitcode("btfsc","(%s >> 3), (%s & 7)",
           AOP(right)->aopu.aop_dir,
           AOP(right)->aopu.aop_dir);
      else
        emitcode("btfss","(%s >> 3), (%s & 7)",
           AOP(right)->aopu.aop_dir,
           AOP(right)->aopu.aop_dir);

      emitcode("xorlw","1");

      /* if the two are equal, then W will be 0 and the Z bit is set
       * we could test Z now, or go ahead and check the high order bytes if
       * the variable we're comparing is larger than a byte. */

      while(--size)
        emitcode("iorwf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));

      if ( IC_TRUE(ifx) ) {
        emitSKPNZ;
        emitcode(" goto","_%05d_DS_",IC_TRUE(ifx)->key+100+labelOffset);
      } else {
        emitSKPZ;
        emitcode(" goto","_%05d_DS_",IC_FALSE(ifx)->key+100+labelOffset);
      }

    } else {
      /* They're both variables that are larger than bits */
      int s = size;

      tlbl = newiTempLabel(NULL);

      while(size--) {

        emitcode("movf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
        emitcode("xorwf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));

        if ( IC_TRUE(ifx) ) {
    if(size) {
      emitSKPZ;
      emitcode(" goto","_%05d_DS_",tlbl->key+100+labelOffset);
    } else {
      emitSKPNZ;
      emitcode(" goto","_%05d_DS_",IC_TRUE(ifx)->key+100+labelOffset);
    }
        } else {
    emitSKPZ;
    emitcode(" goto","_%05d_DS_",IC_FALSE(ifx)->key+100+labelOffset);
        }
        offset++;
      }
      if(s>1 && IC_TRUE(ifx))
        emitcode("","_%05d_DS_:",tlbl->key+100+labelOffset);
    }
        }
        /* mark the icode as generated */
        ifx->generated = 1;
        goto release ;
    }

    /* if they are both bit variables */
    if (AOP_TYPE(left) == AOP_CRY &&
        ((AOP_TYPE(right) == AOP_CRY) || (AOP_TYPE(right) == AOP_LIT))) {
        if(AOP_TYPE(right) == AOP_LIT){
            unsigned long lit = (unsigned long)floatFromVal(AOP(IC_RIGHT(ic))->aopu.aop_lit);
            if(lit == 0L){
                emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
                emitcode("cpl","c");
            } else if(lit == 1L) {
                emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
            } else {
                emitcode("clr","c");
            }
            /* AOP_TYPE(right) == AOP_CRY */
        } else {
            symbol *lbl = newiTempLabel(NULL);
            emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
            emitcode("jb","%s,%05d_DS_",AOP(right)->aopu.aop_dir,(lbl->key+100));
            emitcode("cpl","c");
            emitcode("","%05d_DS_:",(lbl->key+100));
        }
        /* c = 1 if egal */
        if (AOP_TYPE(result) == AOP_CRY && AOP_SIZE(result)){
            outBitC(result);
            goto release ;
        }
        if (ifx) {
            genIfxJump (ifx,"c");
            goto release ;
        }
        /* if the result is used in an arithmetic operation
        then put the result in place */
        outBitC(result);
    } else {
        gencjne(left,right,newiTempLabel(NULL));
        if (AOP_TYPE(result) == AOP_CRY && AOP_SIZE(result)) {
            aopPut(AOP(result),"a",0);
            goto release ;
        }
        if (ifx) {
            genIfxJump (ifx,"a");
            goto release ;
        }
        /* if the result is used in an arithmetic operation
        then put the result in place */
        if (AOP_TYPE(result) != AOP_CRY)
            outAcc(result);
        /* leave the result in acc */
    }

release:
    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* ifxForOp - returns the icode containing the ifx for operand     */
/*-----------------------------------------------------------------*/
static iCode *ifxForOp ( operand *op, iCode *ic )
{
    /* if true symbol then needs to be assigned */
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if (IS_TRUE_SYMOP(op))
        return NULL ;

    /* if this has register type condition and
    the next instruction is ifx with the same operand
    and live to of the operand is upto the ifx only then */
    if (ic->next &&
        ic->next->op == IFX &&
        IC_COND(ic->next)->key == op->key &&
        OP_SYMBOL(op)->liveTo <= ic->next->seq )
        return ic->next;

    return NULL;
}
/*-----------------------------------------------------------------*/
/* genAndOp - for && operation                                     */
/*-----------------------------------------------------------------*/
static void genAndOp (iCode *ic)
{
    operand *left,*right, *result;
    symbol *tlbl;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* note here that && operations that are in an
    if statement are taken away by backPatchLabels
    only those used in arthmetic operations remain */
    aopOp((left=IC_LEFT(ic)),ic,FALSE);
    aopOp((right=IC_RIGHT(ic)),ic,FALSE);
    aopOp((result=IC_RESULT(ic)),ic,FALSE);

    /* if both are bit variables */
    if (AOP_TYPE(left) == AOP_CRY &&
        AOP_TYPE(right) == AOP_CRY ) {
        emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
        emitcode("anl","c,%s",AOP(right)->aopu.aop_dir);
        outBitC(result);
    } else {
        tlbl = newiTempLabel(NULL);
        toBoolean(left);
        emitcode("jz","%05d_DS_",tlbl->key+100);
        toBoolean(right);
        emitcode("","%05d_DS_:",tlbl->key+100);
        outBitAcc(result);
    }

    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}


/*-----------------------------------------------------------------*/
/* genOrOp - for || operation                                      */
/*-----------------------------------------------------------------*/
/*
  tsd pic port -
  modified this code, but it doesn't appear to ever get called
*/

static void genOrOp (iCode *ic)
{
    operand *left,*right, *result;
    symbol *tlbl;

    /* note here that || operations that are in an
    if statement are taken away by backPatchLabels
    only those used in arthmetic operations remain */
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    aopOp((left=IC_LEFT(ic)),ic,FALSE);
    aopOp((right=IC_RIGHT(ic)),ic,FALSE);
    aopOp((result=IC_RESULT(ic)),ic,FALSE);

    /* if both are bit variables */
    if (AOP_TYPE(left) == AOP_CRY &&
        AOP_TYPE(right) == AOP_CRY ) {
      emitcode("clrc","");
      emitcode("btfss","(%s >> 3), (%s & 7)",
         AOP(left)->aopu.aop_dir,
         AOP(left)->aopu.aop_dir);
      emitcode("btfsc","(%s >> 3), (%s & 7)",
         AOP(right)->aopu.aop_dir,
         AOP(right)->aopu.aop_dir);
      emitcode("setc","");

    } else {
        tlbl = newiTempLabel(NULL);
        toBoolean(left);
  emitSKPZ;
        emitcode("goto","%05d_DS_",tlbl->key+100+labelOffset);
        toBoolean(right);
        emitcode("","%05d_DS_:",tlbl->key+100+labelOffset);

        outBitAcc(result);
    }

    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* isLiteralBit - test if lit == 2^n                               */
/*-----------------------------------------------------------------*/
static int isLiteralBit(unsigned long lit)
{
    unsigned long pw[32] = {1L,2L,4L,8L,16L,32L,64L,128L,
    0x100L,0x200L,0x400L,0x800L,
    0x1000L,0x2000L,0x4000L,0x8000L,
    0x10000L,0x20000L,0x40000L,0x80000L,
    0x100000L,0x200000L,0x400000L,0x800000L,
    0x1000000L,0x2000000L,0x4000000L,0x8000000L,
    0x10000000L,0x20000000L,0x40000000L,0x80000000L};
    int idx;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    for(idx = 0; idx < 32; idx++)
        if(lit == pw[idx])
            return idx+1;
    return 0;
}

/*-----------------------------------------------------------------*/
/* continueIfTrue -                                                */
/*-----------------------------------------------------------------*/
static void continueIfTrue (iCode *ic)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(IC_TRUE(ic))
        emitcode("ljmp","%05d_DS_",IC_TRUE(ic)->key+100);
    ic->generated = 1;
}

/*-----------------------------------------------------------------*/
/* jmpIfTrue -                                                     */
/*-----------------------------------------------------------------*/
static void jumpIfTrue (iCode *ic)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(!IC_TRUE(ic))
        emitcode("ljmp","%05d_DS_",IC_FALSE(ic)->key+100);
    ic->generated = 1;
}

/*-----------------------------------------------------------------*/
/* jmpTrueOrFalse -                                                */
/*-----------------------------------------------------------------*/
static void jmpTrueOrFalse (iCode *ic, symbol *tlbl)
{
    // ugly but optimized by peephole
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(IC_TRUE(ic)){
        symbol *nlbl = newiTempLabel(NULL);
        emitcode("sjmp","%05d_DS_",nlbl->key+100);
        emitcode("","%05d_DS_:",tlbl->key+100);
        emitcode("ljmp","%05d_DS_",IC_TRUE(ic)->key+100);
        emitcode("","%05d_DS_:",nlbl->key+100);
    }
    else{
        emitcode("ljmp","%05d_DS_",IC_FALSE(ic)->key+100);
        emitcode("","%05d_DS_:",tlbl->key+100);
    }
    ic->generated = 1;
}

/*-----------------------------------------------------------------*/
/* genAnd  - code for and                                          */
/*-----------------------------------------------------------------*/
static void genAnd (iCode *ic, iCode *ifx)
{
    operand *left, *right, *result;
    int size, offset=0;
    unsigned long lit = 0L;
    int bytelit = 0;
    char buffer[10];

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    aopOp((left = IC_LEFT(ic)),ic,FALSE);
    aopOp((right= IC_RIGHT(ic)),ic,FALSE);
    aopOp((result=IC_RESULT(ic)),ic,TRUE);

#ifdef DEBUG_TYPE
    emitcode("","; Type res[%d] = l[%d]&r[%d]",
             AOP_TYPE(result),
             AOP_TYPE(left), AOP_TYPE(right));
    emitcode("","; Size res[%d] = l[%d]&r[%d]",
             AOP_SIZE(result),
             AOP_SIZE(left), AOP_SIZE(right));
#endif

    /* if left is a literal & right is not then exchange them */
    if ((AOP_TYPE(left) == AOP_LIT && AOP_TYPE(right) != AOP_LIT) ||
  AOP_NEEDSACC(left)) {
        operand *tmp = right ;
        right = left;
        left = tmp;
    }

    /* if result = right then exchange them */
    if(sameRegs(AOP(result),AOP(right))){
        operand *tmp = right ;
        right = left;
        left = tmp;
    }

    /* if right is bit then exchange them */
    if (AOP_TYPE(right) == AOP_CRY &&
        AOP_TYPE(left) != AOP_CRY){
        operand *tmp = right ;
        right = left;
        left = tmp;
    }
    if(AOP_TYPE(right) == AOP_LIT)
        lit = (unsigned long)floatFromVal (AOP(right)->aopu.aop_lit);

    size = AOP_SIZE(result);

    // if(bit & yy)
    // result = bit & yy;
    if (AOP_TYPE(left) == AOP_CRY){
        // c = bit & literal;
        if(AOP_TYPE(right) == AOP_LIT){
            if(lit & 1) {
                if(size && sameRegs(AOP(result),AOP(left)))
                    // no change
                    goto release;
                emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
            } else {
                // bit(result) = 0;
                if(size && (AOP_TYPE(result) == AOP_CRY)){
                    emitcode("clr","%s",AOP(result)->aopu.aop_dir);
                    goto release;
                }
                if((AOP_TYPE(result) == AOP_CRY) && ifx){
                    jumpIfTrue(ifx);
                    goto release;
                }
                emitcode("clr","c");
            }
        } else {
            if (AOP_TYPE(right) == AOP_CRY){
                // c = bit & bit;
                emitcode("mov","c,%s",AOP(right)->aopu.aop_dir);
                emitcode("anl","c,%s",AOP(left)->aopu.aop_dir);
            } else {
                // c = bit & val;
                MOVA(aopGet(AOP(right),0,FALSE,FALSE));
                // c = lsb
                emitcode("rrc","a");
                emitcode("anl","c,%s",AOP(left)->aopu.aop_dir);
            }
        }
        // bit = c
        // val = c
        if(size)
            outBitC(result);
        // if(bit & ...)
        else if((AOP_TYPE(result) == AOP_CRY) && ifx)
            genIfxJump(ifx, "c");
        goto release ;
    }

    // if(val & 0xZZ)       - size = 0, ifx != FALSE  -
    // bit = val & 0xZZ     - size = 1, ifx = FALSE -
    if((AOP_TYPE(right) == AOP_LIT) &&
       (AOP_TYPE(result) == AOP_CRY) &&
       (AOP_TYPE(left) != AOP_CRY)){
        int posbit = isLiteralBit(lit);
        /* left &  2^n */
        if(posbit){
            posbit--;
            MOVA(aopGet(AOP(left),posbit>>3,FALSE,FALSE));
            // bit = left & 2^n
            if(size)
                emitcode("mov","c,acc.%d",posbit&0x07);
            // if(left &  2^n)
            else{
                if(ifx){
                    sprintf(buffer,"acc.%d",posbit&0x07);
                    genIfxJump(ifx, buffer);
                }
                goto release;
            }
        } else {
            symbol *tlbl = newiTempLabel(NULL);
            int sizel = AOP_SIZE(left);
            if(size)
                emitcode("setb","c");
            while(sizel--){
                if((bytelit = ((lit >> (offset*8)) & 0x0FFL)) != 0x0L){
                    MOVA( aopGet(AOP(left),offset,FALSE,FALSE));
                    // byte ==  2^n ?
                    if((posbit = isLiteralBit(bytelit)) != 0)
                        emitcode("jb","acc.%d,%05d_DS_",(posbit-1)&0x07,tlbl->key+100);
                    else{
                        if(bytelit != 0x0FFL)
                            emitcode("anl","a,%s",
                                     aopGet(AOP(right),offset,FALSE,TRUE));
                        emitcode("jnz","%05d_DS_",tlbl->key+100);
                    }
                }
                offset++;
            }
            // bit = left & literal
            if(size){
                emitcode("clr","c");
                emitcode("","%05d_DS_:",tlbl->key+100);
            }
            // if(left & literal)
            else{
                if(ifx)
                    jmpTrueOrFalse(ifx, tlbl);
                goto release ;
            }
        }
        outBitC(result);
        goto release ;
    }

    /* if left is same as result */
    if(sameRegs(AOP(result),AOP(left))){
      for(;size--; offset++,lit>>=8) {
  if(AOP_TYPE(right) == AOP_LIT){
    switch(lit & 0xff) {
    case 0x00:
      /*  and'ing with 0 has clears the result */
      emitcode("clrf","%s",aopGet(AOP(result),offset,FALSE,FALSE));
      break;
    case 0xff:
      emitcode("movf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
      break;

    default:
      {
        int p = my_powof2( (~lit) & 0xff );
        if(p>=0) {
    /* only one bit is set in the literal, so use a bcf instruction */
    emitcode("bcf","%s,%d",aopGet(AOP(left),offset,FALSE,TRUE),p);
        } else {
    emitcode("movlw","0x%x", (lit & 0xff));
    emitcode("andwf","%s,f",aopGet(AOP(left),offset,FALSE,TRUE),p);
        }
      }
    }
  } else {
    if (AOP_TYPE(left) == AOP_ACC)
      emitcode("iorwf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
    else {
      emitcode("movf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
      emitcode("iorwf","%s,f",aopGet(AOP(left),offset,FALSE,FALSE));

    }
  }
      }

    } else {
        // left & result in different registers
        if(AOP_TYPE(result) == AOP_CRY){
            // result = bit
            // if(size), result in bit
            // if(!size && ifx), conditional oper: if(left & right)
            symbol *tlbl = newiTempLabel(NULL);
            int sizer = min(AOP_SIZE(left),AOP_SIZE(right));
            if(size)
                emitcode("setb","c");
            while(sizer--){
                MOVA(aopGet(AOP(right),offset,FALSE,FALSE));
                emitcode("anl","a,%s",
                         aopGet(AOP(left),offset,FALSE,FALSE));
                emitcode("jnz","%05d_DS_",tlbl->key+100);
                offset++;
            }
            if(size){
                CLRC;
                emitcode("","%05d_DS_:",tlbl->key+100);
                outBitC(result);
            } else if(ifx)
                jmpTrueOrFalse(ifx, tlbl);
        } else {
    for(;(size--);offset++) {
      // normal case
      // result = left & right
      if(AOP_TYPE(right) == AOP_LIT){
        int t = (lit >> (offset*8)) & 0x0FFL;
        switch(t) {
        case 0x00:
    emitcode("clrf","%s",
       aopGet(AOP(result),offset,FALSE,FALSE));
    break;
        case 0xff:
    emitcode("movf","%s,w",
       aopGet(AOP(left),offset,FALSE,FALSE));
    emitcode("movwf","%s",
       aopGet(AOP(result),offset,FALSE,FALSE));
    break;
        default:
    emitcode("movlw","0x%x",t);
    emitcode("andwf","%s,w",
       aopGet(AOP(left),offset,FALSE,FALSE));
    emitcode("movwf","%s",
       aopGet(AOP(result),offset,FALSE,FALSE));

        }
        continue;
      }

      if (AOP_TYPE(left) == AOP_ACC)
        emitcode("andwf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
      else {
        emitcode("movf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
        emitcode("andwf","%s,w",
           aopGet(AOP(left),offset,FALSE,FALSE));
      }
      emitcode("movwf","%s",aopGet(AOP(result),offset,FALSE,FALSE));
    }
  }
    }

release :
    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genOr  - code for or                                            */
/*-----------------------------------------------------------------*/
static void genOr (iCode *ic, iCode *ifx)
{
    operand *left, *right, *result;
    int size, offset=0;
    unsigned long lit = 0L;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    aopOp((left = IC_LEFT(ic)),ic,FALSE);
    aopOp((right= IC_RIGHT(ic)),ic,FALSE);
    aopOp((result=IC_RESULT(ic)),ic,TRUE);


    /* if left is a literal & right is not then exchange them */
    if ((AOP_TYPE(left) == AOP_LIT && AOP_TYPE(right) != AOP_LIT) ||
  AOP_NEEDSACC(left)) {
        operand *tmp = right ;
        right = left;
        left = tmp;
    }

    /* if result = right then exchange them */
    if(sameRegs(AOP(result),AOP(right))){
        operand *tmp = right ;
        right = left;
        left = tmp;
    }

    /* if right is bit then exchange them */
    if (AOP_TYPE(right) == AOP_CRY &&
        AOP_TYPE(left) != AOP_CRY){
        operand *tmp = right ;
        right = left;
        left = tmp;
    }

    if(AOP_TYPE(right) == AOP_LIT)
        lit = (unsigned long)floatFromVal (AOP(right)->aopu.aop_lit);

    size = AOP_SIZE(result);

    // if(bit | yy)
    // xx = bit | yy;
    if (AOP_TYPE(left) == AOP_CRY){
        if(AOP_TYPE(right) == AOP_LIT){
            // c = bit & literal;
            if(lit){
                // lit != 0 => result = 1
                if(AOP_TYPE(result) == AOP_CRY){
                    if(size)
                        emitcode("bsf","(%s >> 3), (%s & 7)",
         AOP(result)->aopu.aop_dir,
         AOP(result)->aopu.aop_dir);
                    else if(ifx)
                        continueIfTrue(ifx);
                    goto release;
                }
                emitcode(";XXXsetb","c %s,%d",__FILE__,__LINE__);
            } else {
                // lit == 0 => result = left
                if(size && sameRegs(AOP(result),AOP(left)))
                    goto release;
                emitcode(";XXX mov","c,%s  %s,%d",AOP(left)->aopu.aop_dir,__FILE__,__LINE__);
            }
        } else {
            if (AOP_TYPE(right) == AOP_CRY){
        if(sameRegs(AOP(result),AOP(left))){
                // c = bit | bit;
    emitcode("bcf","(%s >> 3), (%s & 7)",
       AOP(result)->aopu.aop_dir,
       AOP(result)->aopu.aop_dir);
    emitcode("btfsc","(%s >> 3), (%s & 7)",
       AOP(right)->aopu.aop_dir,
       AOP(right)->aopu.aop_dir);
    emitcode("bsf","(%s >> 3), (%s & 7)",
       AOP(result)->aopu.aop_dir,
       AOP(result)->aopu.aop_dir);
        } else {

    emitcode("bcf","(%s >> 3), (%s & 7)",
       AOP(result)->aopu.aop_dir,
       AOP(result)->aopu.aop_dir);
    emitcode("btfss","(%s >> 3), (%s & 7)",
       AOP(right)->aopu.aop_dir,
       AOP(right)->aopu.aop_dir);
    emitcode("btfsc","(%s >> 3), (%s & 7)",
       AOP(left)->aopu.aop_dir,
       AOP(left)->aopu.aop_dir);
    emitcode("bsf","(%s >> 3), (%s & 7)",
       AOP(result)->aopu.aop_dir,
       AOP(result)->aopu.aop_dir);
        }
            }
            else{
                // c = bit | val;
                symbol *tlbl = newiTempLabel(NULL);
                emitcode(";XXX "," %s,%d",__FILE__,__LINE__);
                if(!((AOP_TYPE(result) == AOP_CRY) && ifx))
                    emitcode(";XXX setb","c");
                emitcode(";XXX jb","%s,%05d_DS_",
                         AOP(left)->aopu.aop_dir,tlbl->key+100);
                toBoolean(right);
                emitcode(";XXX jnz","%05d_DS_",tlbl->key+100);
                if((AOP_TYPE(result) == AOP_CRY) && ifx){
                    jmpTrueOrFalse(ifx, tlbl);
                    goto release;
                } else {
                    CLRC;
                    emitcode("","%05d_DS_:",tlbl->key+100);
                }
            }
        }
        // bit = c
        // val = c
        if(size)
            outBitC(result);
        // if(bit | ...)
        else if((AOP_TYPE(result) == AOP_CRY) && ifx)
            genIfxJump(ifx, "c");
        goto release ;
    }

    // if(val | 0xZZ)       - size = 0, ifx != FALSE  -
    // bit = val | 0xZZ     - size = 1, ifx = FALSE -
    if((AOP_TYPE(right) == AOP_LIT) &&
       (AOP_TYPE(result) == AOP_CRY) &&
       (AOP_TYPE(left) != AOP_CRY)){
        if(lit){
    emitcode(";XXX "," %s,%d",__FILE__,__LINE__);
            // result = 1
            if(size)
                emitcode(";XXX setb","%s",AOP(result)->aopu.aop_dir);
            else
                continueIfTrue(ifx);
            goto release;
        } else {
    emitcode(";XXX "," %s,%d",__FILE__,__LINE__);
            // lit = 0, result = boolean(left)
            if(size)
                emitcode(";XXX setb","c");
            toBoolean(right);
            if(size){
                symbol *tlbl = newiTempLabel(NULL);
                emitcode(";XXX jnz","%05d_DS_",tlbl->key+100);
                CLRC;
                emitcode("","%05d_DS_:",tlbl->key+100);
            } else {
                genIfxJump (ifx,"a");
                goto release;
            }
        }
        outBitC(result);
        goto release ;
    }

    /* if left is same as result */
    if(sameRegs(AOP(result),AOP(left))){
      for(;size--; offset++,lit>>=8) {
  if(AOP_TYPE(right) == AOP_LIT){
    if((lit & 0xff) == 0)
      /*  or'ing with 0 has no effect */
      continue;
    else {
      int p = my_powof2(lit & 0xff);
      if(p>=0) {
        /* only one bit is set in the literal, so use a bsf instruction */
        emitcode("bsf","%s,%d",aopGet(AOP(left),offset,FALSE,TRUE),p);
      } else {
        emitcode("movlw","0x%x", (lit & 0xff));
        emitcode("iorwf","%s,f",aopGet(AOP(left),offset,FALSE,TRUE),p);
      }

    }
  } else {
    if (AOP_TYPE(left) == AOP_ACC)
      emitcode("iorwf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
    else {
      emitcode("movf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
      emitcode("iorwf","%s,f",aopGet(AOP(left),offset,FALSE,FALSE));

    }
  }
      }
    } else {
        // left & result in different registers
        if(AOP_TYPE(result) == AOP_CRY){
            // result = bit
            // if(size), result in bit
            // if(!size && ifx), conditional oper: if(left | right)
            symbol *tlbl = newiTempLabel(NULL);
            int sizer = max(AOP_SIZE(left),AOP_SIZE(right));
      emitcode(";XXX "," %s,%d",__FILE__,__LINE__);

            if(size)
                emitcode(";XXX setb","c");
            while(sizer--){
                MOVA(aopGet(AOP(right),offset,FALSE,FALSE));
                emitcode(";XXX orl","a,%s",
                         aopGet(AOP(left),offset,FALSE,FALSE));
                emitcode(";XXX jnz","%05d_DS_",tlbl->key+100);
                offset++;
            }
            if(size){
                CLRC;
                emitcode("","%05d_DS_:",tlbl->key+100);
                outBitC(result);
            } else if(ifx)
                jmpTrueOrFalse(ifx, tlbl);
        } else for(;(size--);offset++){
    // normal case
    // result = left & right
    if(AOP_TYPE(right) == AOP_LIT){
      int t = (lit >> (offset*8)) & 0x0FFL;
      switch(t) {
      case 0x00:
        emitcode("movf","%s,w",
           aopGet(AOP(left),offset,FALSE,FALSE));
        emitcode("movwf","%s",
           aopGet(AOP(result),offset,FALSE,FALSE));
        break;
      default:
        emitcode("movlw","0x%x",t);
        emitcode("iorwf","%s,w",
           aopGet(AOP(left),offset,FALSE,FALSE));
        emitcode("movwf","%s",
           aopGet(AOP(result),offset,FALSE,FALSE));

      }
      continue;
    }

    // faster than result <- left, anl result,right
    // and better if result is SFR
    if (AOP_TYPE(left) == AOP_ACC)
      emitcode("iorwf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
    else {
      emitcode("movf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
      emitcode("iorwf","%s,w",
         aopGet(AOP(left),offset,FALSE,FALSE));
    }
    emitcode("movwf","%s",aopGet(AOP(result),offset,FALSE,FALSE));
  }
    }

release :
    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genXor - code for xclusive or                                   */
/*-----------------------------------------------------------------*/
static void genXor (iCode *ic, iCode *ifx)
{
    operand *left, *right, *result;
    int size, offset=0;
    unsigned long lit = 0L;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    aopOp((left = IC_LEFT(ic)),ic,FALSE);
    aopOp((right= IC_RIGHT(ic)),ic,FALSE);
    aopOp((result=IC_RESULT(ic)),ic,TRUE);

    /* if left is a literal & right is not ||
       if left needs acc & right does not */
    if ((AOP_TYPE(left) == AOP_LIT && AOP_TYPE(right) != AOP_LIT) ||
  (AOP_NEEDSACC(left) && !AOP_NEEDSACC(right))) {
        operand *tmp = right ;
        right = left;
        left = tmp;
    }

    /* if result = right then exchange them */
    if(sameRegs(AOP(result),AOP(right))){
        operand *tmp = right ;
        right = left;
        left = tmp;
    }

    /* if right is bit then exchange them */
    if (AOP_TYPE(right) == AOP_CRY &&
        AOP_TYPE(left) != AOP_CRY){
        operand *tmp = right ;
        right = left;
        left = tmp;
    }
    if(AOP_TYPE(right) == AOP_LIT)
        lit = (unsigned long)floatFromVal (AOP(right)->aopu.aop_lit);

    size = AOP_SIZE(result);

    // if(bit ^ yy)
    // xx = bit ^ yy;
    if (AOP_TYPE(left) == AOP_CRY){
        if(AOP_TYPE(right) == AOP_LIT){
            // c = bit & literal;
            if(lit>>1){
                // lit>>1  != 0 => result = 1
                if(AOP_TYPE(result) == AOP_CRY){
                    if(size)
                        emitcode("setb","%s",AOP(result)->aopu.aop_dir);
                    else if(ifx)
                        continueIfTrue(ifx);
                    goto release;
                }
                emitcode("setb","c");
            } else{
                // lit == (0 or 1)
                if(lit == 0){
                    // lit == 0, result = left
                    if(size && sameRegs(AOP(result),AOP(left)))
                        goto release;
                    emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
                } else{
                    // lit == 1, result = not(left)
                    if(size && sameRegs(AOP(result),AOP(left))){
                        emitcode("cpl","%s",AOP(result)->aopu.aop_dir);
                        goto release;
                    } else {
                        emitcode("mov","c,%s",AOP(left)->aopu.aop_dir);
                        emitcode("cpl","c");
                    }
                }
            }

        } else {
            // right != literal
            symbol *tlbl = newiTempLabel(NULL);
            if (AOP_TYPE(right) == AOP_CRY){
                // c = bit ^ bit;
                emitcode("mov","c,%s",AOP(right)->aopu.aop_dir);
            }
            else{
                int sizer = AOP_SIZE(right);
                // c = bit ^ val
                // if val>>1 != 0, result = 1
                emitcode("setb","c");
                while(sizer){
                    MOVA(aopGet(AOP(right),sizer-1,FALSE,FALSE));
                    if(sizer == 1)
                        // test the msb of the lsb
                        emitcode("anl","a,#0xfe");
                    emitcode("jnz","%05d_DS_",tlbl->key+100);
        sizer--;
                }
                // val = (0,1)
                emitcode("rrc","a");
            }
            emitcode("jnb","%s,%05d_DS_",AOP(left)->aopu.aop_dir,(tlbl->key+100));
            emitcode("cpl","c");
            emitcode("","%05d_DS_:",(tlbl->key+100));
        }
        // bit = c
        // val = c
        if(size)
            outBitC(result);
        // if(bit | ...)
        else if((AOP_TYPE(result) == AOP_CRY) && ifx)
            genIfxJump(ifx, "c");
        goto release ;
    }

    if(sameRegs(AOP(result),AOP(left))){
        /* if left is same as result */
        for(;size--; offset++) {
            if(AOP_TYPE(right) == AOP_LIT){
                if(((lit >> (offset*8)) & 0x0FFL) == 0x00L)
                    continue;
                else
        if (IS_AOP_PREG(left)) {
      MOVA(aopGet(AOP(right),offset,FALSE,FALSE));
      emitcode("xrl","a,%s",aopGet(AOP(left),offset,FALSE,TRUE));
            aopPut(AOP(result),"a",offset);
        } else
      emitcode("xrl","%s,%s",
         aopGet(AOP(left),offset,FALSE,TRUE),
         aopGet(AOP(right),offset,FALSE,FALSE));
            } else {
    if (AOP_TYPE(left) == AOP_ACC)
        emitcode("xrl","a,%s",aopGet(AOP(right),offset,FALSE,FALSE));
    else {
        MOVA(aopGet(AOP(right),offset,FALSE,FALSE));
        if (IS_AOP_PREG(left)) {
      emitcode("xrl","a,%s",aopGet(AOP(left),offset,FALSE,TRUE));
      aopPut(AOP(result),"a",offset);
        } else
      emitcode("xrl","%s,a",
         aopGet(AOP(left),offset,FALSE,TRUE));
    }
            }
        }
    } else {
        // left & result in different registers
        if(AOP_TYPE(result) == AOP_CRY){
            // result = bit
            // if(size), result in bit
            // if(!size && ifx), conditional oper: if(left ^ right)
            symbol *tlbl = newiTempLabel(NULL);
            int sizer = max(AOP_SIZE(left),AOP_SIZE(right));
            if(size)
                emitcode("setb","c");
            while(sizer--){
                if((AOP_TYPE(right) == AOP_LIT) &&
                   (((lit >> (offset*8)) & 0x0FFL) == 0x00L)){
                    MOVA(aopGet(AOP(left),offset,FALSE,FALSE));
                } else {
                    MOVA(aopGet(AOP(right),offset,FALSE,FALSE));
                    emitcode("xrl","a,%s",
                             aopGet(AOP(left),offset,FALSE,FALSE));
                }
                emitcode("jnz","%05d_DS_",tlbl->key+100);
                offset++;
            }
            if(size){
                CLRC;
                emitcode("","%05d_DS_:",tlbl->key+100);
                outBitC(result);
            } else if(ifx)
                jmpTrueOrFalse(ifx, tlbl);
        } else for(;(size--);offset++){
            // normal case
            // result = left & right
            if(AOP_TYPE(right) == AOP_LIT){
        int t = (lit >> (offset*8)) & 0x0FFL;
        switch(t) {
        case 0x00:
    emitcode("movf","%s,w",
       aopGet(AOP(left),offset,FALSE,FALSE));
    emitcode("movwf","%s",
       aopGet(AOP(result),offset,FALSE,FALSE));
    break;
        case 0xff:
    emitcode("comf","%s,w",
       aopGet(AOP(left),offset,FALSE,FALSE));
    emitcode("movwf","%s",
       aopGet(AOP(result),offset,FALSE,FALSE));
    break;
        default:
    emitcode("movlw","0x%x",t);
    emitcode("xorwf","%s,w",
       aopGet(AOP(left),offset,FALSE,FALSE));
    emitcode("movwf","%s",
       aopGet(AOP(result),offset,FALSE,FALSE));

        }
        continue;
            }

            // faster than result <- left, anl result,right
            // and better if result is SFR
      if (AOP_TYPE(left) == AOP_ACC)
    emitcode("xorwf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
      else {
    emitcode("movf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
    emitcode("xorwf","%s,w",aopGet(AOP(left),offset,FALSE,FALSE));
      }
      if ( AOP_TYPE(result) != AOP_ACC)
        emitcode("movwf","%s",aopGet(AOP(result),offset,FALSE,FALSE));
        }
    }

release :
    freeAsmop(left,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(right,NULL,ic,(RESULTONSTACK(ic) ? FALSE : TRUE));
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genInline - write the inline code out                           */
/*-----------------------------------------------------------------*/
static void genInline (iCode *ic)
{
    char buffer[MAX_INLINEASM];
    char *bp = buffer;
    char *bp1= buffer;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    _G.inLine += (!options.asmpeep);
    strcpy(buffer,IC_INLINE(ic));

    /* emit each line as a code */
    while (*bp) {
        if (*bp == '\n') {
            *bp++ = '\0';
            emitcode(bp1,"");
            bp1 = bp;
        } else {
            if (*bp == ':') {
                bp++;
                *bp = '\0';
                bp++;
                emitcode(bp1,"");
                bp1 = bp;
            } else
                bp++;
        }
    }
    if (bp1 != bp)
        emitcode(bp1,"");
    /*     emitcode("",buffer); */
    _G.inLine -= (!options.asmpeep);
}

/*-----------------------------------------------------------------*/
/* genRRC - rotate right with carry                                */
/*-----------------------------------------------------------------*/
static void genRRC (iCode *ic)
{
    operand *left , *result ;
    int size, offset = 0;
    char *l;

    /* rotate right with carry */
    left = IC_LEFT(ic);
    result=IC_RESULT(ic);
    aopOp (left,ic,FALSE);
    aopOp (result,ic,FALSE);

    /* move it to the result */
    size = AOP_SIZE(result);
    offset = size - 1 ;
    CLRC;
    while (size--) {
        l = aopGet(AOP(left),offset,FALSE,FALSE);
        MOVA(l);
        emitcode("rrc","a");
        if (AOP_SIZE(result) > 1)
            aopPut(AOP(result),"a",offset--);
    }
    /* now we need to put the carry into the
    highest order byte of the result */
    if (AOP_SIZE(result) > 1) {
        l = aopGet(AOP(result),AOP_SIZE(result)-1,FALSE,FALSE);
        MOVA(l);
    }
    emitcode("mov","acc.7,c");
    aopPut(AOP(result),"a",AOP_SIZE(result)-1);
    freeAsmop(left,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genRLC - generate code for rotate left with carry               */
/*-----------------------------------------------------------------*/
static void genRLC (iCode *ic)
{
    operand *left , *result ;
    int size, offset = 0;
    char *l;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* rotate right with carry */
    left = IC_LEFT(ic);
    result=IC_RESULT(ic);
    aopOp (left,ic,FALSE);
    aopOp (result,ic,FALSE);

    /* move it to the result */
    size = AOP_SIZE(result);
    offset = 0 ;
    if (size--) {
        l = aopGet(AOP(left),offset,FALSE,FALSE);
        MOVA(l);
        emitcode("add","a,acc");
        if (AOP_SIZE(result) > 1)
            aopPut(AOP(result),"a",offset++);
        while (size--) {
            l = aopGet(AOP(left),offset,FALSE,FALSE);
            MOVA(l);
            emitcode("rlc","a");
            if (AOP_SIZE(result) > 1)
                aopPut(AOP(result),"a",offset++);
        }
    }
    /* now we need to put the carry into the
    highest order byte of the result */
    if (AOP_SIZE(result) > 1) {
        l = aopGet(AOP(result),0,FALSE,FALSE);
        MOVA(l);
    }
    emitcode("mov","acc.0,c");
    aopPut(AOP(result),"a",0);
    freeAsmop(left,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genGetHbit - generates code get highest order bit               */
/*-----------------------------------------------------------------*/
static void genGetHbit (iCode *ic)
{
    operand *left, *result;
    left = IC_LEFT(ic);
    result=IC_RESULT(ic);
    aopOp (left,ic,FALSE);
    aopOp (result,ic,FALSE);

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* get the highest order byte into a */
    MOVA(aopGet(AOP(left),AOP_SIZE(left) - 1,FALSE,FALSE));
    if(AOP_TYPE(result) == AOP_CRY){
        emitcode("rlc","a");
        outBitC(result);
    }
    else{
        emitcode("rl","a");
        emitcode("anl","a,#0x01");
        outAcc(result);
    }


    freeAsmop(left,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* AccRol - rotate left accumulator by known count                 */
/*-----------------------------------------------------------------*/
static void AccRol (int shCount)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    shCount &= 0x0007;              // shCount : 0..7
    switch(shCount){
        case 0 :
            break;
        case 1 :
            emitcode("rl","a");
            break;
        case 2 :
            emitcode("rl","a");
            emitcode("rl","a");
            break;
        case 3 :
            emitcode("swap","a");
            emitcode("rr","a");
            break;
        case 4 :
            emitcode("swap","a");
            break;
        case 5 :
            emitcode("swap","a");
            emitcode("rl","a");
            break;
        case 6 :
            emitcode("rr","a");
            emitcode("rr","a");
            break;
        case 7 :
            emitcode("rr","a");
            break;
    }
}

/*-----------------------------------------------------------------*/
/* AccLsh - left shift accumulator by known count                  */
/*-----------------------------------------------------------------*/
static void AccLsh (int shCount)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(shCount != 0){
        if(shCount == 1)
            emitcode("add","a,acc");
        else
      if(shCount == 2) {
            emitcode("add","a,acc");
            emitcode("add","a,acc");
        } else {
            /* rotate left accumulator */
            AccRol(shCount);
            /* and kill the lower order bits */
            emitcode("anl","a,#0x%02x", SLMask[shCount]);
        }
    }
}

/*-----------------------------------------------------------------*/
/* AccRsh - right shift accumulator by known count                 */
/*-----------------------------------------------------------------*/
static void AccRsh (int shCount)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(shCount != 0){
        if(shCount == 1){
            CLRC;
            emitcode("rrc","a");
        } else {
            /* rotate right accumulator */
            AccRol(8 - shCount);
            /* and kill the higher order bits */
            emitcode("anl","a,#0x%02x", SRMask[shCount]);
        }
    }
}

/*-----------------------------------------------------------------*/
/* AccSRsh - signed right shift accumulator by known count                 */
/*-----------------------------------------------------------------*/
static void AccSRsh (int shCount)
{
    symbol *tlbl ;
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(shCount != 0){
        if(shCount == 1){
            emitcode("mov","c,acc.7");
            emitcode("rrc","a");
        } else if(shCount == 2){
            emitcode("mov","c,acc.7");
            emitcode("rrc","a");
            emitcode("mov","c,acc.7");
            emitcode("rrc","a");
        } else {
            tlbl = newiTempLabel(NULL);
            /* rotate right accumulator */
            AccRol(8 - shCount);
            /* and kill the higher order bits */
            emitcode("anl","a,#0x%02x", SRMask[shCount]);
            emitcode("jnb","acc.%d,%05d_DS_",7-shCount,tlbl->key+100);
            emitcode("orl","a,#0x%02x",
                     (unsigned char)~SRMask[shCount]);
            emitcode("","%05d_DS_:",tlbl->key+100);
        }
    }
}

/*-----------------------------------------------------------------*/
/* shiftR1Left2Result - shift right one byte from left to result   */
/*-----------------------------------------------------------------*/
static void shiftR1Left2Result (operand *left, int offl,
                                operand *result, int offr,
                                int shCount, int sign)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    MOVA(aopGet(AOP(left),offl,FALSE,FALSE));
    /* shift right accumulator */
    if(sign)
        AccSRsh(shCount);
    else
        AccRsh(shCount);
    aopPut(AOP(result),"a",offr);
}

/*-----------------------------------------------------------------*/
/* shiftL1Left2Result - shift left one byte from left to result    */
/*-----------------------------------------------------------------*/
static void shiftL1Left2Result (operand *left, int offl,
                                operand *result, int offr, int shCount)
{
    char *l;
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    l = aopGet(AOP(left),offl,FALSE,FALSE);
    MOVA(l);
    /* shift left accumulator */
    AccLsh(shCount);
    aopPut(AOP(result),"a",offr);
}

/*-----------------------------------------------------------------*/
/* movLeft2Result - move byte from left to result                  */
/*-----------------------------------------------------------------*/
static void movLeft2Result (operand *left, int offl,
                            operand *result, int offr, int sign)
{
    char *l;
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(!sameRegs(AOP(left),AOP(result)) || (offl != offr)){
        l = aopGet(AOP(left),offl,FALSE,FALSE);

        if (*l == '@' && (IS_AOP_PREG(result))) {
            emitcode("mov","a,%s",l);
            aopPut(AOP(result),"a",offr);
        } else {
            if(!sign)
                aopPut(AOP(result),l,offr);
            else{
                /* MSB sign in acc.7 ! */
                if(getDataSize(left) == offl+1){
                    emitcode("mov","a,%s",l);
                    aopPut(AOP(result),"a",offr);
                }
            }
        }
    }
}

/*-----------------------------------------------------------------*/
/* AccAXRrl1 - right rotate c->a:x->c by 1                         */
/*-----------------------------------------------------------------*/
static void AccAXRrl1 (char *x)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    emitcode("rrc","a");
    emitcode("xch","a,%s", x);
    emitcode("rrc","a");
    emitcode("xch","a,%s", x);
}

/*-----------------------------------------------------------------*/
/* AccAXLrl1 - left rotate c<-a:x<-c by 1                          */
/*-----------------------------------------------------------------*/
static void AccAXLrl1 (char *x)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    emitcode("xch","a,%s",x);
    emitcode("rlc","a");
    emitcode("xch","a,%s",x);
    emitcode("rlc","a");
}

/*-----------------------------------------------------------------*/
/* AccAXLsh1 - left shift a:x<-0 by 1                              */
/*-----------------------------------------------------------------*/
static void AccAXLsh1 (char *x)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    emitcode("xch","a,%s",x);
    emitcode("add","a,acc");
    emitcode("xch","a,%s",x);
    emitcode("rlc","a");
}

/*-----------------------------------------------------------------*/
/* AccAXLsh - left shift a:x by known count (0..7)                 */
/*-----------------------------------------------------------------*/
static void AccAXLsh (char *x, int shCount)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    switch(shCount){
        case 0 :
            break;
        case 1 :
            AccAXLsh1(x);
            break;
        case 2 :
            AccAXLsh1(x);
            AccAXLsh1(x);
            break;
        case 3 :
        case 4 :
        case 5 :                        // AAAAABBB:CCCCCDDD
            AccRol(shCount);            // BBBAAAAA:CCCCCDDD
            emitcode("anl","a,#0x%02x",
                     SLMask[shCount]);  // BBB00000:CCCCCDDD
            emitcode("xch","a,%s",x);   // CCCCCDDD:BBB00000
            AccRol(shCount);            // DDDCCCCC:BBB00000
            emitcode("xch","a,%s",x);   // BBB00000:DDDCCCCC
            emitcode("xrl","a,%s",x);   // (BBB^DDD)CCCCC:DDDCCCCC
            emitcode("xch","a,%s",x);   // DDDCCCCC:(BBB^DDD)CCCCC
            emitcode("anl","a,#0x%02x",
                     SLMask[shCount]);  // DDD00000:(BBB^DDD)CCCCC
            emitcode("xch","a,%s",x);   // (BBB^DDD)CCCCC:DDD00000
            emitcode("xrl","a,%s",x);   // BBBCCCCC:DDD00000
            break;
        case 6 :                        // AAAAAABB:CCCCCCDD
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 000000BB:CCCCCCDD
            emitcode("mov","c,acc.0");  // c = B
            emitcode("xch","a,%s",x);   // CCCCCCDD:000000BB
            AccAXRrl1(x);               // BCCCCCCD:D000000B
            AccAXRrl1(x);               // BBCCCCCC:DD000000
            break;
        case 7 :                        // a:x <<= 7
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 0000000B:CCCCCCCD
            emitcode("mov","c,acc.0");  // c = B
            emitcode("xch","a,%s",x);   // CCCCCCCD:0000000B
            AccAXRrl1(x);               // BCCCCCCC:D0000000
            break;
        default :
            break;
    }
}

/*-----------------------------------------------------------------*/
/* AccAXRsh - right shift a:x known count (0..7)                   */
/*-----------------------------------------------------------------*/
static void AccAXRsh (char *x, int shCount)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    switch(shCount){
        case 0 :
            break;
        case 1 :
            CLRC;
            AccAXRrl1(x);               // 0->a:x
            break;
        case 2 :
            CLRC;
            AccAXRrl1(x);               // 0->a:x
            CLRC;
            AccAXRrl1(x);               // 0->a:x
            break;
        case 3 :
        case 4 :
        case 5 :                        // AAAAABBB:CCCCCDDD = a:x
            AccRol(8 - shCount);        // BBBAAAAA:DDDCCCCC
            emitcode("xch","a,%s",x);   // CCCCCDDD:BBBAAAAA
            AccRol(8 - shCount);        // DDDCCCCC:BBBAAAAA
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 000CCCCC:BBBAAAAA
            emitcode("xrl","a,%s",x);   // BBB(CCCCC^AAAAA):BBBAAAAA
            emitcode("xch","a,%s",x);   // BBBAAAAA:BBB(CCCCC^AAAAA)
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 000AAAAA:BBB(CCCCC^AAAAA)
            emitcode("xch","a,%s",x);   // BBB(CCCCC^AAAAA):000AAAAA
            emitcode("xrl","a,%s",x);   // BBBCCCCC:000AAAAA
            emitcode("xch","a,%s",x);   // 000AAAAA:BBBCCCCC
            break;
        case 6 :                        // AABBBBBB:CCDDDDDD
            emitcode("mov","c,acc.7");
            AccAXLrl1(x);               // ABBBBBBC:CDDDDDDA
            AccAXLrl1(x);               // BBBBBBCC:DDDDDDAA
            emitcode("xch","a,%s",x);   // DDDDDDAA:BBBBBBCC
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 000000AA:BBBBBBCC
            break;
        case 7 :                        // ABBBBBBB:CDDDDDDD
            emitcode("mov","c,acc.7");  // c = A
            AccAXLrl1(x);               // BBBBBBBC:DDDDDDDA
            emitcode("xch","a,%s",x);   // DDDDDDDA:BBBBBBCC
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 0000000A:BBBBBBBC
            break;
        default :
            break;
    }
}

/*-----------------------------------------------------------------*/
/* AccAXRshS - right shift signed a:x known count (0..7)           */
/*-----------------------------------------------------------------*/
static void AccAXRshS (char *x, int shCount)
{
    symbol *tlbl ;
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    switch(shCount){
        case 0 :
            break;
        case 1 :
            emitcode("mov","c,acc.7");
            AccAXRrl1(x);               // s->a:x
            break;
        case 2 :
            emitcode("mov","c,acc.7");
            AccAXRrl1(x);               // s->a:x
            emitcode("mov","c,acc.7");
            AccAXRrl1(x);               // s->a:x
            break;
        case 3 :
        case 4 :
        case 5 :                        // AAAAABBB:CCCCCDDD = a:x
            tlbl = newiTempLabel(NULL);
            AccRol(8 - shCount);        // BBBAAAAA:CCCCCDDD
            emitcode("xch","a,%s",x);   // CCCCCDDD:BBBAAAAA
            AccRol(8 - shCount);        // DDDCCCCC:BBBAAAAA
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 000CCCCC:BBBAAAAA
            emitcode("xrl","a,%s",x);   // BBB(CCCCC^AAAAA):BBBAAAAA
            emitcode("xch","a,%s",x);   // BBBAAAAA:BBB(CCCCC^AAAAA)
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 000AAAAA:BBB(CCCCC^AAAAA)
            emitcode("xch","a,%s",x);   // BBB(CCCCC^AAAAA):000AAAAA
            emitcode("xrl","a,%s",x);   // BBBCCCCC:000AAAAA
            emitcode("xch","a,%s",x);   // 000SAAAA:BBBCCCCC
            emitcode("jnb","acc.%d,%05d_DS_",7-shCount,tlbl->key+100);
            emitcode("orl","a,#0x%02x",
                     (unsigned char)~SRMask[shCount]);  // 111AAAAA:BBBCCCCC
            emitcode("","%05d_DS_:",tlbl->key+100);
            break;                      // SSSSAAAA:BBBCCCCC
        case 6 :                        // AABBBBBB:CCDDDDDD
            tlbl = newiTempLabel(NULL);
            emitcode("mov","c,acc.7");
            AccAXLrl1(x);               // ABBBBBBC:CDDDDDDA
            AccAXLrl1(x);               // BBBBBBCC:DDDDDDAA
            emitcode("xch","a,%s",x);   // DDDDDDAA:BBBBBBCC
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 000000AA:BBBBBBCC
            emitcode("jnb","acc.%d,%05d_DS_",7-shCount,tlbl->key+100);
            emitcode("orl","a,#0x%02x",
                     (unsigned char)~SRMask[shCount]);  // 111111AA:BBBBBBCC
            emitcode("","%05d_DS_:",tlbl->key+100);
            break;
        case 7 :                        // ABBBBBBB:CDDDDDDD
            tlbl = newiTempLabel(NULL);
            emitcode("mov","c,acc.7");  // c = A
            AccAXLrl1(x);               // BBBBBBBC:DDDDDDDA
            emitcode("xch","a,%s",x);   // DDDDDDDA:BBBBBBCC
            emitcode("anl","a,#0x%02x",
                     SRMask[shCount]);  // 0000000A:BBBBBBBC
            emitcode("jnb","acc.%d,%05d_DS_",7-shCount,tlbl->key+100);
            emitcode("orl","a,#0x%02x",
                     (unsigned char)~SRMask[shCount]);  // 1111111A:BBBBBBBC
            emitcode("","%05d_DS_:",tlbl->key+100);
            break;
        default :
            break;
    }
}

/*-----------------------------------------------------------------*/
/* shiftL2Left2Result - shift left two bytes from left to result   */
/*-----------------------------------------------------------------*/
static void shiftL2Left2Result (operand *left, int offl,
                                operand *result, int offr, int shCount)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(sameRegs(AOP(result), AOP(left)) &&
       ((offl + MSB16) == offr)){
  /* don't crash result[offr] */
  MOVA(aopGet(AOP(left),offl,FALSE,FALSE));
  emitcode("xch","a,%s", aopGet(AOP(left),offl+MSB16,FALSE,FALSE));
    } else {
  movLeft2Result(left,offl, result, offr, 0);
  MOVA(aopGet(AOP(left),offl+MSB16,FALSE,FALSE));
    }
    /* ax << shCount (x = lsb(result))*/
    AccAXLsh( aopGet(AOP(result),offr,FALSE,FALSE) , shCount);
    aopPut(AOP(result),"a",offr+MSB16);
}


/*-----------------------------------------------------------------*/
/* shiftR2Left2Result - shift right two bytes from left to result  */
/*-----------------------------------------------------------------*/
static void shiftR2Left2Result (operand *left, int offl,
                                operand *result, int offr,
                                int shCount, int sign)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(sameRegs(AOP(result), AOP(left)) &&
       ((offl + MSB16) == offr)){
  /* don't crash result[offr] */
  MOVA(aopGet(AOP(left),offl,FALSE,FALSE));
  emitcode("xch","a,%s", aopGet(AOP(left),offl+MSB16,FALSE,FALSE));
    } else {
  movLeft2Result(left,offl, result, offr, 0);
  MOVA(aopGet(AOP(left),offl+MSB16,FALSE,FALSE));
    }
    /* a:x >> shCount (x = lsb(result))*/
    if(sign)
        AccAXRshS( aopGet(AOP(result),offr,FALSE,FALSE) , shCount);
    else
        AccAXRsh( aopGet(AOP(result),offr,FALSE,FALSE) , shCount);
    if(getDataSize(result) > 1)
        aopPut(AOP(result),"a",offr+MSB16);
}

/*-----------------------------------------------------------------*/
/* shiftLLeftOrResult - shift left one byte from left, or to result*/
/*-----------------------------------------------------------------*/
static void shiftLLeftOrResult (operand *left, int offl,
                                operand *result, int offr, int shCount)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    MOVA(aopGet(AOP(left),offl,FALSE,FALSE));
    /* shift left accumulator */
    AccLsh(shCount);
    /* or with result */
    emitcode("orl","a,%s", aopGet(AOP(result),offr,FALSE,FALSE));
    /* back to result */
    aopPut(AOP(result),"a",offr);
}

/*-----------------------------------------------------------------*/
/* shiftRLeftOrResult - shift right one byte from left,or to result*/
/*-----------------------------------------------------------------*/
static void shiftRLeftOrResult (operand *left, int offl,
                                operand *result, int offr, int shCount)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    MOVA(aopGet(AOP(left),offl,FALSE,FALSE));
    /* shift right accumulator */
    AccRsh(shCount);
    /* or with result */
    emitcode("orl","a,%s", aopGet(AOP(result),offr,FALSE,FALSE));
    /* back to result */
    aopPut(AOP(result),"a",offr);
}

/*-----------------------------------------------------------------*/
/* genlshOne - left shift a one byte quantity by known count       */
/*-----------------------------------------------------------------*/
static void genlshOne (operand *result, operand *left, int shCount)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    shiftL1Left2Result(left, LSB, result, LSB, shCount);
}

/*-----------------------------------------------------------------*/
/* genlshTwo - left shift two bytes by known amount != 0           */
/*-----------------------------------------------------------------*/
static void genlshTwo (operand *result,operand *left, int shCount)
{
    int size;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    size = getDataSize(result);

    /* if shCount >= 8 */
    if (shCount >= 8) {
        shCount -= 8 ;

        if (size > 1){
            if (shCount)
                shiftL1Left2Result(left, LSB, result, MSB16, shCount);
            else
                movLeft2Result(left, LSB, result, MSB16, 0);
        }
        aopPut(AOP(result),zero,LSB);
    }

    /*  1 <= shCount <= 7 */
    else {
        if(size == 1)
            shiftL1Left2Result(left, LSB, result, LSB, shCount);
        else
            shiftL2Left2Result(left, LSB, result, LSB, shCount);
    }
}

/*-----------------------------------------------------------------*/
/* shiftLLong - shift left one long from left to result            */
/* offl = LSB or MSB16                                             */
/*-----------------------------------------------------------------*/
static void shiftLLong (operand *left, operand *result, int offr )
{
    char *l;
    int size = AOP_SIZE(result);

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(size >= LSB+offr){
        l = aopGet(AOP(left),LSB,FALSE,FALSE);
        MOVA(l);
        emitcode("add","a,acc");
  if (sameRegs(AOP(left),AOP(result)) &&
      size >= MSB16+offr && offr != LSB )
      emitcode("xch","a,%s",
         aopGet(AOP(left),LSB+offr,FALSE,FALSE));
  else
      aopPut(AOP(result),"a",LSB+offr);
    }

    if(size >= MSB16+offr){
  if (!(sameRegs(AOP(result),AOP(left)) && size >= MSB16+offr && offr != LSB) ) {
      l = aopGet(AOP(left),MSB16,FALSE,FALSE);
      MOVA(l);
  }
        emitcode("rlc","a");
  if (sameRegs(AOP(left),AOP(result)) &&
      size >= MSB24+offr && offr != LSB)
      emitcode("xch","a,%s",
         aopGet(AOP(left),MSB16+offr,FALSE,FALSE));
  else
      aopPut(AOP(result),"a",MSB16+offr);
    }

    if(size >= MSB24+offr){
  if (!(sameRegs(AOP(left),AOP(left)) && size >= MSB24+offr && offr != LSB)) {
      l = aopGet(AOP(left),MSB24,FALSE,FALSE);
      MOVA(l);
  }
        emitcode("rlc","a");
  if (sameRegs(AOP(left),AOP(result)) &&
      size >= MSB32+offr && offr != LSB )
      emitcode("xch","a,%s",
         aopGet(AOP(left),MSB24+offr,FALSE,FALSE));
  else
      aopPut(AOP(result),"a",MSB24+offr);
    }

    if(size > MSB32+offr){
  if (!(sameRegs(AOP(result),AOP(left)) && size >= MSB32+offr && offr != LSB)) {
      l = aopGet(AOP(left),MSB32,FALSE,FALSE);
      MOVA(l);
  }
        emitcode("rlc","a");
        aopPut(AOP(result),"a",MSB32+offr);
    }
    if(offr != LSB)
        aopPut(AOP(result),zero,LSB);
}

/*-----------------------------------------------------------------*/
/* genlshFour - shift four byte by a known amount != 0             */
/*-----------------------------------------------------------------*/
static void genlshFour (operand *result, operand *left, int shCount)
{
    int size;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    size = AOP_SIZE(result);

    /* if shifting more that 3 bytes */
    if (shCount >= 24 ) {
        shCount -= 24;
        if (shCount)
            /* lowest order of left goes to the highest
            order of the destination */
            shiftL1Left2Result(left, LSB, result, MSB32, shCount);
        else
            movLeft2Result(left, LSB, result, MSB32, 0);
        aopPut(AOP(result),zero,LSB);
        aopPut(AOP(result),zero,MSB16);
        aopPut(AOP(result),zero,MSB32);
        return;
    }

    /* more than two bytes */
    else if ( shCount >= 16 ) {
        /* lower order two bytes goes to higher order two bytes */
        shCount -= 16;
        /* if some more remaining */
        if (shCount)
            shiftL2Left2Result(left, LSB, result, MSB24, shCount);
        else {
            movLeft2Result(left, MSB16, result, MSB32, 0);
            movLeft2Result(left, LSB, result, MSB24, 0);
        }
        aopPut(AOP(result),zero,MSB16);
        aopPut(AOP(result),zero,LSB);
        return;
    }

    /* if more than 1 byte */
    else if ( shCount >= 8 ) {
        /* lower order three bytes goes to higher order  three bytes */
        shCount -= 8;
        if(size == 2){
            if(shCount)
                shiftL1Left2Result(left, LSB, result, MSB16, shCount);
            else
                movLeft2Result(left, LSB, result, MSB16, 0);
        }
        else{   /* size = 4 */
            if(shCount == 0){
                movLeft2Result(left, MSB24, result, MSB32, 0);
                movLeft2Result(left, MSB16, result, MSB24, 0);
                movLeft2Result(left, LSB, result, MSB16, 0);
                aopPut(AOP(result),zero,LSB);
            }
            else if(shCount == 1)
                shiftLLong(left, result, MSB16);
            else{
                shiftL2Left2Result(left, MSB16, result, MSB24, shCount);
                shiftL1Left2Result(left, LSB, result, MSB16, shCount);
                shiftRLeftOrResult(left, LSB, result, MSB24, 8 - shCount);
                aopPut(AOP(result),zero,LSB);
            }
        }
    }

    /* 1 <= shCount <= 7 */
    else if(shCount <= 2){
        shiftLLong(left, result, LSB);
        if(shCount == 2)
            shiftLLong(result, result, LSB);
    }
    /* 3 <= shCount <= 7, optimize */
    else{
        shiftL2Left2Result(left, MSB24, result, MSB24, shCount);
        shiftRLeftOrResult(left, MSB16, result, MSB24, 8 - shCount);
        shiftL2Left2Result(left, LSB, result, LSB, shCount);
    }
}

/*-----------------------------------------------------------------*/
/* genLeftShiftLiteral - left shifting by known count              */
/*-----------------------------------------------------------------*/
static void genLeftShiftLiteral (operand *left,
                                 operand *right,
                                 operand *result,
                                 iCode *ic)
{
    int shCount = (int) floatFromVal (AOP(right)->aopu.aop_lit);
    int size;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    freeAsmop(right,NULL,ic,TRUE);

    aopOp(left,ic,FALSE);
    aopOp(result,ic,FALSE);

    size = getSize(operandType(result));

#if VIEW_SIZE
    emitcode("; shift left ","result %d, left %d",size,
             AOP_SIZE(left));
#endif

    /* I suppose that the left size >= result size */
    if(shCount == 0){
        while(size--){
            movLeft2Result(left, size, result, size, 0);
        }
    }

    else if(shCount >= (size * 8))
        while(size--)
            aopPut(AOP(result),zero,size);
    else{
        switch (size) {
            case 1:
                genlshOne (result,left,shCount);
                break;

            case 2:
            case 3:
                genlshTwo (result,left,shCount);
                break;

            case 4:
                genlshFour (result,left,shCount);
                break;
        }
    }
    freeAsmop(left,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genLeftShift - generates code for left shifting                 */
/*-----------------------------------------------------------------*/
static void genLeftShift (iCode *ic)
{
    operand *left,*right, *result;
    int size, offset;
    char *l;
    symbol *tlbl , *tlbl1;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    right = IC_RIGHT(ic);
    left  = IC_LEFT(ic);
    result = IC_RESULT(ic);

    aopOp(right,ic,FALSE);

    /* if the shift count is known then do it
    as efficiently as possible */
    if (AOP_TYPE(right) == AOP_LIT) {
        genLeftShiftLiteral (left,right,result,ic);
        return ;
    }

    /* shift count is unknown then we have to form
    a loop get the loop count in B : Note: we take
    only the lower order byte since shifting
    more that 32 bits make no sense anyway, ( the
    largest size of an object can be only 32 bits ) */

    emitcode("mov","b,%s",aopGet(AOP(right),0,FALSE,FALSE));
    emitcode("inc","b");
    freeAsmop (right,NULL,ic,TRUE);
    aopOp(left,ic,FALSE);
    aopOp(result,ic,FALSE);

    /* now move the left to the result if they are not the
    same */
    if (!sameRegs(AOP(left),AOP(result)) &&
        AOP_SIZE(result) > 1) {

        size = AOP_SIZE(result);
        offset=0;
        while (size--) {
            l = aopGet(AOP(left),offset,FALSE,TRUE);
            if (*l == '@' && (IS_AOP_PREG(result))) {

                emitcode("mov","a,%s",l);
                aopPut(AOP(result),"a",offset);
            } else
                aopPut(AOP(result),l,offset);
            offset++;
        }
    }

    tlbl = newiTempLabel(NULL);
    size = AOP_SIZE(result);
    offset = 0 ;
    tlbl1 = newiTempLabel(NULL);

    /* if it is only one byte then */
    if (size == 1) {
  symbol *tlbl1 = newiTempLabel(NULL);

        l = aopGet(AOP(left),0,FALSE,FALSE);
        MOVA(l);
  emitcode("sjmp","%05d_DS_",tlbl1->key+100);
        emitcode("","%05d_DS_:",tlbl->key+100);
        emitcode("add","a,acc");
  emitcode("","%05d_DS_:",tlbl1->key+100);
        emitcode("djnz","b,%05d_DS_",tlbl->key+100);
        aopPut(AOP(result),"a",0);
        goto release ;
    }

    reAdjustPreg(AOP(result));

    emitcode("sjmp","%05d_DS_",tlbl1->key+100);
    emitcode("","%05d_DS_:",tlbl->key+100);
    l = aopGet(AOP(result),offset,FALSE,FALSE);
    MOVA(l);
    emitcode("add","a,acc");
    aopPut(AOP(result),"a",offset++);
    while (--size) {
        l = aopGet(AOP(result),offset,FALSE,FALSE);
        MOVA(l);
        emitcode("rlc","a");
        aopPut(AOP(result),"a",offset++);
    }
    reAdjustPreg(AOP(result));

    emitcode("","%05d_DS_:",tlbl1->key+100);
    emitcode("djnz","b,%05d_DS_",tlbl->key+100);
release:
    freeAsmop(left,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genrshOne - right shift a one byte quantity by known count      */
/*-----------------------------------------------------------------*/
static void genrshOne (operand *result, operand *left,
                       int shCount, int sign)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    shiftR1Left2Result(left, LSB, result, LSB, shCount, sign);
}

/*-----------------------------------------------------------------*/
/* genrshTwo - right shift two bytes by known amount != 0          */
/*-----------------------------------------------------------------*/
static void genrshTwo (operand *result,operand *left,
                       int shCount, int sign)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if shCount >= 8 */
    if (shCount >= 8) {
        shCount -= 8 ;
        if (shCount)
            shiftR1Left2Result(left, MSB16, result, LSB,
                               shCount, sign);
        else
            movLeft2Result(left, MSB16, result, LSB, sign);
        addSign(result, MSB16, sign);
    }

    /*  1 <= shCount <= 7 */
    else
        shiftR2Left2Result(left, LSB, result, LSB, shCount, sign);
}

/*-----------------------------------------------------------------*/
/* shiftRLong - shift right one long from left to result           */
/* offl = LSB or MSB16                                             */
/*-----------------------------------------------------------------*/
static void shiftRLong (operand *left, int offl,
                        operand *result, int sign)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    if(!sign)
        emitcode("clr","c");
    MOVA(aopGet(AOP(left),MSB32,FALSE,FALSE));
    if(sign)
        emitcode("mov","c,acc.7");
    emitcode("rrc","a");
    aopPut(AOP(result),"a",MSB32-offl);
    if(offl == MSB16)
        /* add sign of "a" */
        addSign(result, MSB32, sign);

    MOVA(aopGet(AOP(left),MSB24,FALSE,FALSE));
    emitcode("rrc","a");
    aopPut(AOP(result),"a",MSB24-offl);

    MOVA(aopGet(AOP(left),MSB16,FALSE,FALSE));
    emitcode("rrc","a");
    aopPut(AOP(result),"a",MSB16-offl);

    if(offl == LSB){
        MOVA(aopGet(AOP(left),LSB,FALSE,FALSE));
        emitcode("rrc","a");
        aopPut(AOP(result),"a",LSB);
    }
}

/*-----------------------------------------------------------------*/
/* genrshFour - shift four byte by a known amount != 0             */
/*-----------------------------------------------------------------*/
static void genrshFour (operand *result, operand *left,
                        int shCount, int sign)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if shifting more that 3 bytes */
    if(shCount >= 24 ) {
        shCount -= 24;
        if(shCount)
            shiftR1Left2Result(left, MSB32, result, LSB, shCount, sign);
        else
            movLeft2Result(left, MSB32, result, LSB, sign);
        addSign(result, MSB16, sign);
    }
    else if(shCount >= 16){
        shCount -= 16;
        if(shCount)
            shiftR2Left2Result(left, MSB24, result, LSB, shCount, sign);
        else{
            movLeft2Result(left, MSB24, result, LSB, 0);
            movLeft2Result(left, MSB32, result, MSB16, sign);
        }
        addSign(result, MSB24, sign);
    }
    else if(shCount >= 8){
        shCount -= 8;
        if(shCount == 1)
            shiftRLong(left, MSB16, result, sign);
        else if(shCount == 0){
            movLeft2Result(left, MSB16, result, LSB, 0);
            movLeft2Result(left, MSB24, result, MSB16, 0);
            movLeft2Result(left, MSB32, result, MSB24, sign);
            addSign(result, MSB32, sign);
        }
        else{
            shiftR2Left2Result(left, MSB16, result, LSB, shCount, 0);
            shiftLLeftOrResult(left, MSB32, result, MSB16, 8 - shCount);
            /* the last shift is signed */
            shiftR1Left2Result(left, MSB32, result, MSB24, shCount, sign);
            addSign(result, MSB32, sign);
        }
    }
    else{   /* 1 <= shCount <= 7 */
        if(shCount <= 2){
            shiftRLong(left, LSB, result, sign);
            if(shCount == 2)
                shiftRLong(result, LSB, result, sign);
        }
        else{
            shiftR2Left2Result(left, LSB, result, LSB, shCount, 0);
            shiftLLeftOrResult(left, MSB24, result, MSB16, 8 - shCount);
            shiftR2Left2Result(left, MSB24, result, MSB24, shCount, sign);
        }
    }
}

/*-----------------------------------------------------------------*/
/* genRightShiftLiteral - right shifting by known count            */
/*-----------------------------------------------------------------*/
static void genRightShiftLiteral (operand *left,
                                  operand *right,
                                  operand *result,
                                  iCode *ic,
                                  int sign)
{
    int shCount = (int) floatFromVal (AOP(right)->aopu.aop_lit);
    int size;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    freeAsmop(right,NULL,ic,TRUE);

    aopOp(left,ic,FALSE);
    aopOp(result,ic,FALSE);

#if VIEW_SIZE
    emitcode("; shift right ","result %d, left %d",AOP_SIZE(result),
             AOP_SIZE(left));
#endif

    size = getDataSize(left);
    /* test the LEFT size !!! */

    /* I suppose that the left size >= result size */
    if(shCount == 0){
        size = getDataSize(result);
        while(size--)
            movLeft2Result(left, size, result, size, 0);
    }

    else if(shCount >= (size * 8)){
        if(sign)
            /* get sign in acc.7 */
            MOVA(aopGet(AOP(left),size-1,FALSE,FALSE));
        addSign(result, LSB, sign);
    } else{
        switch (size) {
            case 1:
                genrshOne (result,left,shCount,sign);
                break;

            case 2:
                genrshTwo (result,left,shCount,sign);
                break;

            case 4:
                genrshFour (result,left,shCount,sign);
                break;
            default :
                break;
        }

        freeAsmop(left,NULL,ic,TRUE);
        freeAsmop(result,NULL,ic,TRUE);
    }
}

/*-----------------------------------------------------------------*/
/* genSignedRightShift - right shift of signed number              */
/*-----------------------------------------------------------------*/
static void genSignedRightShift (iCode *ic)
{
    operand *right, *left, *result;
    int size, offset;
    char *l;
    symbol *tlbl, *tlbl1 ;

    /* we do it the hard way put the shift count in b
    and loop thru preserving the sign */
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    right = IC_RIGHT(ic);
    left  = IC_LEFT(ic);
    result = IC_RESULT(ic);

    aopOp(right,ic,FALSE);


    if ( AOP_TYPE(right) == AOP_LIT) {
  genRightShiftLiteral (left,right,result,ic,1);
  return ;
    }
        /* shift count is unknown then we have to form
       a loop get the loop count in B : Note: we take
       only the lower order byte since shifting
       more that 32 bits make no sense anyway, ( the
       largest size of an object can be only 32 bits ) */

    emitcode("mov","b,%s",aopGet(AOP(right),0,FALSE,FALSE));
    emitcode("inc","b");
    freeAsmop (right,NULL,ic,TRUE);
    aopOp(left,ic,FALSE);
    aopOp(result,ic,FALSE);

    /* now move the left to the result if they are not the
    same */
    if (!sameRegs(AOP(left),AOP(result)) &&
        AOP_SIZE(result) > 1) {

        size = AOP_SIZE(result);
        offset=0;
        while (size--) {
            l = aopGet(AOP(left),offset,FALSE,TRUE);
            if (*l == '@' && IS_AOP_PREG(result)) {

                emitcode("mov","a,%s",l);
                aopPut(AOP(result),"a",offset);
            } else
                aopPut(AOP(result),l,offset);
            offset++;
        }
    }

    /* mov the highest order bit to OVR */
    tlbl = newiTempLabel(NULL);
    tlbl1= newiTempLabel(NULL);

    size = AOP_SIZE(result);
    offset = size - 1;
    emitcode("mov","a,%s",aopGet(AOP(left),offset,FALSE,FALSE));
    emitcode("rlc","a");
    emitcode("mov","ov,c");
    /* if it is only one byte then */
    if (size == 1) {
        l = aopGet(AOP(left),0,FALSE,FALSE);
        MOVA(l);
  emitcode("sjmp","%05d_DS_",tlbl1->key+100);
        emitcode("","%05d_DS_:",tlbl->key+100);
        emitcode("mov","c,ov");
        emitcode("rrc","a");
  emitcode("","%05d_DS_:",tlbl1->key+100);
        emitcode("djnz","b,%05d_DS_",tlbl->key+100);
        aopPut(AOP(result),"a",0);
        goto release ;
    }

    reAdjustPreg(AOP(result));
    emitcode("sjmp","%05d_DS_",tlbl1->key+100);
    emitcode("","%05d_DS_:",tlbl->key+100);
    emitcode("mov","c,ov");
    while (size--) {
        l = aopGet(AOP(result),offset,FALSE,FALSE);
        MOVA(l);
        emitcode("rrc","a");
        aopPut(AOP(result),"a",offset--);
    }
    reAdjustPreg(AOP(result));
    emitcode("","%05d_DS_:",tlbl1->key+100);
    emitcode("djnz","b,%05d_DS_",tlbl->key+100);

release:
    freeAsmop(left,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genRightShift - generate code for right shifting                */
/*-----------------------------------------------------------------*/
static void genRightShift (iCode *ic)
{
    operand *right, *left, *result;
    sym_link *retype ;
    int size, offset;
    char *l;
    symbol *tlbl, *tlbl1 ;

    /* if signed then we do it the hard way preserve the
    sign bit moving it inwards */
    retype = getSpec(operandType(IC_RESULT(ic)));
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    if (!SPEC_USIGN(retype)) {
        genSignedRightShift (ic);
        return ;
    }

    /* signed & unsigned types are treated the same : i.e. the
    signed is NOT propagated inwards : quoting from the
    ANSI - standard : "for E1 >> E2, is equivalent to division
    by 2**E2 if unsigned or if it has a non-negative value,
    otherwise the result is implementation defined ", MY definition
    is that the sign does not get propagated */

    right = IC_RIGHT(ic);
    left  = IC_LEFT(ic);
    result = IC_RESULT(ic);

    aopOp(right,ic,FALSE);

    /* if the shift count is known then do it
    as efficiently as possible */
    if (AOP_TYPE(right) == AOP_LIT) {
        genRightShiftLiteral (left,right,result,ic, 0);
        return ;
    }

    /* shift count is unknown then we have to form
    a loop get the loop count in B : Note: we take
    only the lower order byte since shifting
    more that 32 bits make no sense anyway, ( the
    largest size of an object can be only 32 bits ) */

    emitcode("mov","b,%s",aopGet(AOP(right),0,FALSE,FALSE));
    emitcode("inc","b");
    freeAsmop (right,NULL,ic,TRUE);
    aopOp(left,ic,FALSE);
    aopOp(result,ic,FALSE);

    /* now move the left to the result if they are not the
    same */
    if (!sameRegs(AOP(left),AOP(result)) &&
        AOP_SIZE(result) > 1) {

        size = AOP_SIZE(result);
        offset=0;
        while (size--) {
            l = aopGet(AOP(left),offset,FALSE,TRUE);
            if (*l == '@' && IS_AOP_PREG(result)) {

                emitcode("mov","a,%s",l);
                aopPut(AOP(result),"a",offset);
            } else
                aopPut(AOP(result),l,offset);
            offset++;
        }
    }

    tlbl = newiTempLabel(NULL);
    tlbl1= newiTempLabel(NULL);
    size = AOP_SIZE(result);
    offset = size - 1;

    /* if it is only one byte then */
    if (size == 1) {
        l = aopGet(AOP(left),0,FALSE,FALSE);
        MOVA(l);
  emitcode("sjmp","%05d_DS_",tlbl1->key+100);
        emitcode("","%05d_DS_:",tlbl->key+100);
        CLRC;
        emitcode("rrc","a");
  emitcode("","%05d_DS_:",tlbl1->key+100);
        emitcode("djnz","b,%05d_DS_",tlbl->key+100);
        aopPut(AOP(result),"a",0);
        goto release ;
    }

    reAdjustPreg(AOP(result));
    emitcode("sjmp","%05d_DS_",tlbl1->key+100);
    emitcode("","%05d_DS_:",tlbl->key+100);
    CLRC;
    while (size--) {
        l = aopGet(AOP(result),offset,FALSE,FALSE);
        MOVA(l);
        emitcode("rrc","a");
        aopPut(AOP(result),"a",offset--);
    }
    reAdjustPreg(AOP(result));

    emitcode("","%05d_DS_:",tlbl1->key+100);
    emitcode("djnz","b,%05d_DS_",tlbl->key+100);

release:
    freeAsmop(left,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genUnpackBits - generates code for unpacking bits               */
/*-----------------------------------------------------------------*/
static void genUnpackBits (operand *result, char *rname, int ptype)
{
    int shCnt ;
    int rlen = 0 ;
    sym_link *etype;
    int offset = 0 ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    etype = getSpec(operandType(result));

    /* read the first byte  */
    switch (ptype) {

    case POINTER:
    case IPOINTER:
  emitcode("mov","a,@%s",rname);
  break;

    case PPOINTER:
  emitcode("movx","a,@%s",rname);
  break;

    case FPOINTER:
  emitcode("movx","a,@dptr");
  break;

    case CPOINTER:
  emitcode("clr","a");
  emitcode("movc","a","@a+dptr");
  break;

    case GPOINTER:
  emitcode("lcall","__gptrget");
  break;
    }

    /* if we have bitdisplacement then it fits   */
    /* into this byte completely or if length is */
    /* less than a byte                          */
    if ((shCnt = SPEC_BSTR(etype)) ||
        (SPEC_BLEN(etype) <= 8))  {

        /* shift right acc */
        AccRsh(shCnt);

        emitcode("anl","a,#0x%02x",
                 ((unsigned char) -1)>>(8 - SPEC_BLEN(etype)));
        aopPut(AOP(result),"a",offset);
        return ;
    }

    /* bit field did not fit in a byte  */
    rlen = SPEC_BLEN(etype) - 8;
    aopPut(AOP(result),"a",offset++);

    while (1)  {

  switch (ptype) {
  case POINTER:
  case IPOINTER:
      emitcode("inc","%s",rname);
      emitcode("mov","a,@%s",rname);
      break;

  case PPOINTER:
      emitcode("inc","%s",rname);
      emitcode("movx","a,@%s",rname);
      break;

  case FPOINTER:
      emitcode("inc","dptr");
      emitcode("movx","a,@dptr");
      break;

  case CPOINTER:
      emitcode("clr","a");
      emitcode("inc","dptr");
      emitcode("movc","a","@a+dptr");
      break;

  case GPOINTER:
      emitcode("inc","dptr");
      emitcode("lcall","__gptrget");
      break;
  }

  rlen -= 8;
  /* if we are done */
  if ( rlen <= 0 )
      break ;

  aopPut(AOP(result),"a",offset++);

    }

    if (rlen) {
  emitcode("anl","a,#0x%02x",((unsigned char)-1)>>(-rlen));
  aopPut(AOP(result),"a",offset);
    }

    return ;
}


/*-----------------------------------------------------------------*/
/* genDataPointerGet - generates code when ptr offset is known     */
/*-----------------------------------------------------------------*/
static void genDataPointerGet (operand *left,
             operand *result,
             iCode *ic)
{
    char *l;
    char buffer[256];
    int size , offset = 0;
    aopOp(result,ic,TRUE);

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    /* get the string representation of the name */
    l = aopGet(AOP(left),0,FALSE,TRUE);
    size = AOP_SIZE(result);
    // tsd, was l+1 - the underline `_' prefix was being stripped
    while (size--) {
  if (offset)
      sprintf(buffer,"(%s + %d)",l,offset);
  else
      sprintf(buffer,"%s",l);
  aopPut(AOP(result),buffer,offset++);
    }

    freeAsmop(left,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genNearPointerGet - emitcode for near pointer fetch             */
/*-----------------------------------------------------------------*/
static void genNearPointerGet (operand *left,
             operand *result,
             iCode *ic)
{
    asmop *aop = NULL;
    regs *preg = NULL ;
    char *rname ;
    sym_link *rtype, *retype;
    sym_link *ltype = operandType(left);
    char buffer[80];

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    rtype = operandType(result);
    retype= getSpec(rtype);

    aopOp(left,ic,FALSE);

    /* if left is rematerialisable and
       result is not bit variable type and
       the left is pointer to data space i.e
       lower 128 bytes of space */
    if (AOP_TYPE(left) == AOP_IMMD &&
  !IS_BITVAR(retype)         &&
  DCL_TYPE(ltype) == POINTER) {
  genDataPointerGet (left,result,ic);
  return ;
    }

  /* if the value is already in a pointer register
       then don't need anything more */
    if (!AOP_INPREG(AOP(left))) {
  /* otherwise get a free pointer register */
  aop = newAsmop(0);
  preg = getFreePtr(ic,&aop,FALSE);
  emitcode("mov","%s,%s",
    preg->name,
    aopGet(AOP(left),0,FALSE,TRUE));
  rname = preg->name ;
    } else
  rname = aopGet(AOP(left),0,FALSE,FALSE);

    freeAsmop(left,NULL,ic,TRUE);
    aopOp (result,ic,FALSE);

      /* if bitfield then unpack the bits */
    if (IS_BITVAR(retype))
  genUnpackBits (result,rname,POINTER);
    else {
  /* we have can just get the values */
  int size = AOP_SIZE(result);
  int offset = 0 ;

  while (size--) {
      if (IS_AOP_PREG(result) || AOP_TYPE(result) == AOP_STK ) {

    emitcode("mov","a,@%s",rname);
    aopPut(AOP(result),"a",offset);
      } else {
    sprintf(buffer,"@%s",rname);
    aopPut(AOP(result),buffer,offset);
      }
      offset++ ;
      if (size)
    emitcode("inc","%s",rname);
  }
    }

    /* now some housekeeping stuff */
    if (aop) {
  /* we had to allocate for this iCode */
  freeAsmop(NULL,aop,ic,TRUE);
    } else {
  /* we did not allocate which means left
     already in a pointer register, then
     if size > 0 && this could be used again
     we have to point it back to where it
     belongs */
  if (AOP_SIZE(result) > 1 &&
      !OP_SYMBOL(left)->remat &&
      ( OP_SYMBOL(left)->liveTo > ic->seq ||
        ic->depth )) {
      int size = AOP_SIZE(result) - 1;
      while (size--)
    emitcode("dec","%s",rname);
  }
    }

    /* done */
    freeAsmop(result,NULL,ic,TRUE);

}

/*-----------------------------------------------------------------*/
/* genPagedPointerGet - emitcode for paged pointer fetch           */
/*-----------------------------------------------------------------*/
static void genPagedPointerGet (operand *left,
             operand *result,
             iCode *ic)
{
    asmop *aop = NULL;
    regs *preg = NULL ;
    char *rname ;
    sym_link *rtype, *retype;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    rtype = operandType(result);
    retype= getSpec(rtype);

    aopOp(left,ic,FALSE);

  /* if the value is already in a pointer register
       then don't need anything more */
    if (!AOP_INPREG(AOP(left))) {
  /* otherwise get a free pointer register */
  aop = newAsmop(0);
  preg = getFreePtr(ic,&aop,FALSE);
  emitcode("mov","%s,%s",
    preg->name,
    aopGet(AOP(left),0,FALSE,TRUE));
  rname = preg->name ;
    } else
  rname = aopGet(AOP(left),0,FALSE,FALSE);

    freeAsmop(left,NULL,ic,TRUE);
    aopOp (result,ic,FALSE);

    /* if bitfield then unpack the bits */
    if (IS_BITVAR(retype))
  genUnpackBits (result,rname,PPOINTER);
    else {
  /* we have can just get the values */
  int size = AOP_SIZE(result);
  int offset = 0 ;

  while (size--) {

      emitcode("movx","a,@%s",rname);
      aopPut(AOP(result),"a",offset);

      offset++ ;

      if (size)
    emitcode("inc","%s",rname);
  }
    }

    /* now some housekeeping stuff */
    if (aop) {
  /* we had to allocate for this iCode */
  freeAsmop(NULL,aop,ic,TRUE);
    } else {
  /* we did not allocate which means left
     already in a pointer register, then
     if size > 0 && this could be used again
     we have to point it back to where it
     belongs */
  if (AOP_SIZE(result) > 1 &&
      !OP_SYMBOL(left)->remat &&
      ( OP_SYMBOL(left)->liveTo > ic->seq ||
        ic->depth )) {
      int size = AOP_SIZE(result) - 1;
      while (size--)
    emitcode("dec","%s",rname);
  }
    }

    /* done */
    freeAsmop(result,NULL,ic,TRUE);


}

/*-----------------------------------------------------------------*/
/* genFarPointerGet - gget value from far space                    */
/*-----------------------------------------------------------------*/
static void genFarPointerGet (operand *left,
                              operand *result, iCode *ic)
{
    int size, offset ;
    sym_link *retype = getSpec(operandType(result));

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    aopOp(left,ic,FALSE);

    /* if the operand is already in dptr
    then we do nothing else we move the value to dptr */
    if (AOP_TYPE(left) != AOP_STR) {
        /* if this is remateriazable */
        if (AOP_TYPE(left) == AOP_IMMD)
            emitcode("mov","dptr,%s",aopGet(AOP(left),0,TRUE,FALSE));
        else { /* we need to get it byte by byte */
            emitcode("mov","dpl,%s",aopGet(AOP(left),0,FALSE,FALSE));
            emitcode("mov","dph,%s",aopGet(AOP(left),1,FALSE,FALSE));
            if (options.model == MODEL_FLAT24)
            {
               emitcode("mov", "dpx,%s",aopGet(AOP(left),2,FALSE,FALSE));
            }
        }
    }
    /* so dptr know contains the address */
    freeAsmop(left,NULL,ic,TRUE);
    aopOp(result,ic,FALSE);

    /* if bit then unpack */
    if (IS_BITVAR(retype))
        genUnpackBits(result,"dptr",FPOINTER);
    else {
        size = AOP_SIZE(result);
        offset = 0 ;

        while (size--) {
            emitcode("movx","a,@dptr");
            aopPut(AOP(result),"a",offset++);
            if (size)
                emitcode("inc","dptr");
        }
    }

    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* emitcodePointerGet - gget value from code space                  */
/*-----------------------------------------------------------------*/
static void emitcodePointerGet (operand *left,
                                operand *result, iCode *ic)
{
    int size, offset ;
    sym_link *retype = getSpec(operandType(result));

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    aopOp(left,ic,FALSE);

    /* if the operand is already in dptr
    then we do nothing else we move the value to dptr */
    if (AOP_TYPE(left) != AOP_STR) {
        /* if this is remateriazable */
        if (AOP_TYPE(left) == AOP_IMMD)
            emitcode("mov","dptr,%s",aopGet(AOP(left),0,TRUE,FALSE));
        else { /* we need to get it byte by byte */
            emitcode("mov","dpl,%s",aopGet(AOP(left),0,FALSE,FALSE));
            emitcode("mov","dph,%s",aopGet(AOP(left),1,FALSE,FALSE));
            if (options.model == MODEL_FLAT24)
            {
               emitcode("mov", "dpx,%s",aopGet(AOP(left),2,FALSE,FALSE));
            }
        }
    }
    /* so dptr know contains the address */
    freeAsmop(left,NULL,ic,TRUE);
    aopOp(result,ic,FALSE);

    /* if bit then unpack */
    if (IS_BITVAR(retype))
        genUnpackBits(result,"dptr",CPOINTER);
    else {
        size = AOP_SIZE(result);
        offset = 0 ;

        while (size--) {
            emitcode("clr","a");
            emitcode("movc","a,@a+dptr");
            aopPut(AOP(result),"a",offset++);
            if (size)
                emitcode("inc","dptr");
        }
    }

    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genGenPointerGet - gget value from generic pointer space        */
/*-----------------------------------------------------------------*/
static void genGenPointerGet (operand *left,
                              operand *result, iCode *ic)
{
    int size, offset ;
    sym_link *retype = getSpec(operandType(result));

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    aopOp(left,ic,FALSE);

    /* if the operand is already in dptr
    then we do nothing else we move the value to dptr */
    if (AOP_TYPE(left) != AOP_STR) {
        /* if this is remateriazable */
        if (AOP_TYPE(left) == AOP_IMMD) {
            emitcode("mov","dptr,%s",aopGet(AOP(left),0,TRUE,FALSE));
      emitcode("mov","b,#%d",pointerCode(retype));
  }
        else { /* we need to get it byte by byte */

    emitcode("movf","%s,w",aopGet(AOP(left),0,FALSE,FALSE));
    emitcode("movwf","FSR");
    /*
            emitcode("mov","dpl,%s",aopGet(AOP(left),0,FALSE,FALSE));
            emitcode("mov","dph,%s",aopGet(AOP(left),1,FALSE,FALSE));
            if (options.model == MODEL_FLAT24)
            {
               emitcode("mov", "dpx,%s",aopGet(AOP(left),2,FALSE,FALSE));
               emitcode("mov","b,%s",aopGet(AOP(left),3,FALSE,FALSE));
            }
            else
            {
              emitcode("mov","b,%s",aopGet(AOP(left),2,FALSE,FALSE));
            }
    */
        }
    }
    /* so dptr know contains the address */
    freeAsmop(left,NULL,ic,TRUE);
    aopOp(result,ic,FALSE);

    /* if bit then unpack */
    if (IS_BITVAR(retype))
        genUnpackBits(result,"dptr",GPOINTER);
    else {
        size = AOP_SIZE(result);
        offset = 0 ;

        while (size--) {
    //emitcode("lcall","__gptrget");
            emitcode("movf","indf,w");
            //aopPut(AOP(result),"a",offset++);
      emitcode("movwf","%s",
         aopGet(AOP(result),offset++,FALSE,FALSE));
            if (size)
                emitcode("incf","fsr,f");
        }
    }

    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genPointerGet - generate code for pointer get                   */
/*-----------------------------------------------------------------*/
static void genPointerGet (iCode *ic)
{
    operand *left, *result ;
    sym_link *type, *etype;
    int p_type;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    left = IC_LEFT(ic);
    result = IC_RESULT(ic) ;

    /* depending on the type of pointer we need to
    move it to the correct pointer register */
    type = operandType(left);
    etype = getSpec(type);
    /* if left is of type of pointer then it is simple */
    if (IS_PTR(type) && !IS_FUNC(type->next))
        p_type = DCL_TYPE(type);
    else {
  /* we have to go by the storage class */
  p_type = PTR_TYPE(SPEC_OCLS(etype));

/*  if (SPEC_OCLS(etype)->codesp ) { */
/*      p_type = CPOINTER ;  */
/*  } */
/*  else */
/*      if (SPEC_OCLS(etype)->fmap && !SPEC_OCLS(etype)->paged) */
/*    p_type = FPOINTER ; */
/*      else */
/*    if (SPEC_OCLS(etype)->fmap && SPEC_OCLS(etype)->paged) */
/*        p_type = PPOINTER; */
/*    else */
/*        if (SPEC_OCLS(etype) == idata ) */
/*      p_type = IPOINTER; */
/*        else */
/*      p_type = POINTER ; */
    }

    /* now that we have the pointer type we assign
    the pointer values */
    switch (p_type) {

    case POINTER:
    case IPOINTER:
  genNearPointerGet (left,result,ic);
  break;

    case PPOINTER:
  genPagedPointerGet(left,result,ic);
  break;

    case FPOINTER:
  genFarPointerGet (left,result,ic);
  break;

    case CPOINTER:
  emitcodePointerGet (left,result,ic);
  break;

    case GPOINTER:
  genGenPointerGet (left,result,ic);
  break;
    }

}

/*-----------------------------------------------------------------*/
/* genPackBits - generates code for packed bit storage             */
/*-----------------------------------------------------------------*/
static void genPackBits (sym_link    *etype ,
                         operand *right ,
                         char *rname, int p_type)
{
    int shCount = 0 ;
    int offset = 0  ;
    int rLen = 0 ;
    int blen, bstr ;
    char *l ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    blen = SPEC_BLEN(etype);
    bstr = SPEC_BSTR(etype);

    l = aopGet(AOP(right),offset++,FALSE,FALSE);
    MOVA(l);

    /* if the bit lenth is less than or    */
    /* it exactly fits a byte then         */
    if (SPEC_BLEN(etype) <= 8 )  {
        shCount = SPEC_BSTR(etype) ;

        /* shift left acc */
        AccLsh(shCount);

        if (SPEC_BLEN(etype) < 8 ) { /* if smaller than a byte */


            switch (p_type) {
                case POINTER:
                    emitcode ("mov","b,a");
                    emitcode("mov","a,@%s",rname);
                    break;

                case FPOINTER:
                    emitcode ("mov","b,a");
                    emitcode("movx","a,@dptr");
                    break;

                case GPOINTER:
                    emitcode ("push","b");
                    emitcode ("push","acc");
                    emitcode ("lcall","__gptrget");
                    emitcode ("pop","b");
                    break;
            }

            emitcode ("anl","a,#0x%02x",(unsigned char)
                      ((unsigned char)(0xFF << (blen+bstr)) |
                       (unsigned char)(0xFF >> (8-bstr)) ) );
            emitcode ("orl","a,b");
            if (p_type == GPOINTER)
                emitcode("pop","b");
        }
    }

    switch (p_type) {
        case POINTER:
            emitcode("mov","@%s,a",rname);
            break;

        case FPOINTER:
            emitcode("movx","@dptr,a");
            break;

        case GPOINTER:
            DEBUGemitcode(";lcall","__gptrput");
            break;
    }

    /* if we r done */
    if ( SPEC_BLEN(etype) <= 8 )
        return ;

    emitcode("inc","%s",rname);
    rLen = SPEC_BLEN(etype) ;

    /* now generate for lengths greater than one byte */
    while (1) {

        l = aopGet(AOP(right),offset++,FALSE,TRUE);

        rLen -= 8 ;
        if (rLen <= 0 )
            break ;

        switch (p_type) {
            case POINTER:
                if (*l == '@') {
                    MOVA(l);
                    emitcode("mov","@%s,a",rname);
                } else
                    emitcode("mov","@%s,%s",rname,l);
                break;

            case FPOINTER:
                MOVA(l);
                emitcode("movx","@dptr,a");
                break;

            case GPOINTER:
                MOVA(l);
                DEBUGemitcode(";lcall","__gptrput");
                break;
        }
        emitcode ("inc","%s",rname);
    }

    MOVA(l);

    /* last last was not complete */
    if (rLen)   {
        /* save the byte & read byte */
        switch (p_type) {
            case POINTER:
                emitcode ("mov","b,a");
                emitcode("mov","a,@%s",rname);
                break;

            case FPOINTER:
                emitcode ("mov","b,a");
                emitcode("movx","a,@dptr");
                break;

            case GPOINTER:
                emitcode ("push","b");
                emitcode ("push","acc");
                emitcode ("lcall","__gptrget");
                emitcode ("pop","b");
                break;
        }

        emitcode ("anl","a,#0x%02x",((unsigned char)-1 << -rLen) );
        emitcode ("orl","a,b");
    }

    if (p_type == GPOINTER)
        emitcode("pop","b");

    switch (p_type) {

    case POINTER:
  emitcode("mov","@%s,a",rname);
  break;

    case FPOINTER:
  emitcode("movx","@dptr,a");
  break;

    case GPOINTER:
  DEBUGemitcode(";lcall","__gptrput");
  break;
    }
}
/*-----------------------------------------------------------------*/
/* genDataPointerSet - remat pointer to data space                 */
/*-----------------------------------------------------------------*/
static void genDataPointerSet(operand *right,
            operand *result,
            iCode *ic)
{
    int size, offset = 0 ;
    char *l, buffer[256];

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    aopOp(right,ic,FALSE);

    l = aopGet(AOP(result),0,FALSE,TRUE);
    size = AOP_SIZE(right);
    // tsd, was l+1 - the underline `_' prefix was being stripped
    while (size--) {
  if (offset)
      sprintf(buffer,"(%s + %d)",l,offset);
  else
      sprintf(buffer,"%s",l);

  if (AOP_TYPE(right) == AOP_LIT) {
    unsigned int lit = floatFromVal (AOP(IC_RIGHT(ic))->aopu.aop_lit);
    lit = lit >> (8*offset);
    if(lit) {
      emitcode("movlw","%s",lit);
      emitcode("movwf","%s",buffer);
    } else
      emitcode("clrf","%s",buffer);
  }else {
    emitcode("movf","%s,w",aopGet(AOP(right),offset,FALSE,FALSE));
    emitcode("movwf","%s",buffer);
  }

  offset++;
    }

    freeAsmop(right,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genNearPointerSet - emitcode for near pointer put                */
/*-----------------------------------------------------------------*/
static void genNearPointerSet (operand *right,
                               operand *result,
                               iCode *ic)
{
    asmop *aop = NULL;
    char *l;
    sym_link *retype;
    sym_link *ptype = operandType(result);


    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    retype= getSpec(operandType(right));

    aopOp(result,ic,FALSE);

    /* if the result is rematerializable &
       in data space & not a bit variable */
    if (AOP_TYPE(result) == AOP_IMMD &&
  DCL_TYPE(ptype) == POINTER   &&
  !IS_BITVAR(retype)) {
  genDataPointerSet (right,result,ic);
  return;
    }

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    /* if the value is already in a pointer register
    then don't need anything more */
    if (!AOP_INPREG(AOP(result))) {
        /* otherwise get a free pointer register */
        //aop = newAsmop(0);
        //preg = getFreePtr(ic,&aop,FALSE);
  DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
  //emitcode("mov","%s,%s",
        //         preg->name,
        //         aopGet(AOP(result),0,FALSE,TRUE));
        //rname = preg->name ;
  emitcode("movwf","fsr");
    }// else
    //   rname = aopGet(AOP(result),0,FALSE,FALSE);

    freeAsmop(result,NULL,ic,TRUE);
    aopOp (right,ic,FALSE);
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    /* if bitfield then unpack the bits */
    if (IS_BITVAR(retype)) {
      werror(E_INTERNAL_ERROR,__FILE__,__LINE__,
       "The programmer is obviously confused");
      //genPackBits (retype,right,rname,POINTER);
      exit(1);
    }
    else {
        /* we have can just get the values */
        int size = AOP_SIZE(right);
        int offset = 0 ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
        while (size--) {
            l = aopGet(AOP(right),offset,FALSE,TRUE);
            if (*l == '@' ) {
        //MOVA(l);
        //emitcode("mov","@%s,a",rname);
        emitcode("movf","indf,w ;1");
            } else {

        if (AOP_TYPE(right) == AOP_LIT) {
    unsigned int lit = floatFromVal (AOP(IC_RIGHT(ic))->aopu.aop_lit);
    if(lit) {
      emitcode("movlw","%s",l);
      emitcode("movwf","indf ;2");
    } else
      emitcode("clrf","indf");
        }else {
    emitcode("movf","%s,w",l);
    emitcode("movwf","indf ;2");
        }
      //emitcode("mov","@%s,%s",rname,l);
      }
            if (size)
        emitcode("incf","fsr,f ;3");
      //emitcode("inc","%s",rname);
            offset++;
        }
    }

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* now some housekeeping stuff */
    if (aop) {
        /* we had to allocate for this iCode */
        freeAsmop(NULL,aop,ic,TRUE);
    } else {
        /* we did not allocate which means left
        already in a pointer register, then
        if size > 0 && this could be used again
        we have to point it back to where it
        belongs */
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
        if (AOP_SIZE(right) > 1 &&
            !OP_SYMBOL(result)->remat &&
            ( OP_SYMBOL(result)->liveTo > ic->seq ||
              ic->depth )) {
            int size = AOP_SIZE(right) - 1;
            while (size--)
        emitcode("decf","fsr,f");
        //emitcode("dec","%s",rname);
        }
    }

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* done */
    freeAsmop(right,NULL,ic,TRUE);


}

/*-----------------------------------------------------------------*/
/* genPagedPointerSet - emitcode for Paged pointer put             */
/*-----------------------------------------------------------------*/
static void genPagedPointerSet (operand *right,
             operand *result,
             iCode *ic)
{
    asmop *aop = NULL;
    regs *preg = NULL ;
    char *rname , *l;
    sym_link *retype;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    retype= getSpec(operandType(right));

    aopOp(result,ic,FALSE);

    /* if the value is already in a pointer register
       then don't need anything more */
    if (!AOP_INPREG(AOP(result))) {
  /* otherwise get a free pointer register */
  aop = newAsmop(0);
  preg = getFreePtr(ic,&aop,FALSE);
  emitcode("mov","%s,%s",
    preg->name,
    aopGet(AOP(result),0,FALSE,TRUE));
  rname = preg->name ;
    } else
  rname = aopGet(AOP(result),0,FALSE,FALSE);

    freeAsmop(result,NULL,ic,TRUE);
    aopOp (right,ic,FALSE);

    /* if bitfield then unpack the bits */
    if (IS_BITVAR(retype))
  genPackBits (retype,right,rname,PPOINTER);
    else {
  /* we have can just get the values */
  int size = AOP_SIZE(right);
  int offset = 0 ;

  while (size--) {
      l = aopGet(AOP(right),offset,FALSE,TRUE);

      MOVA(l);
      emitcode("movx","@%s,a",rname);

      if (size)
    emitcode("inc","%s",rname);

      offset++;
  }
    }

    /* now some housekeeping stuff */
    if (aop) {
  /* we had to allocate for this iCode */
  freeAsmop(NULL,aop,ic,TRUE);
    } else {
  /* we did not allocate which means left
     already in a pointer register, then
     if size > 0 && this could be used again
     we have to point it back to where it
     belongs */
  if (AOP_SIZE(right) > 1 &&
      !OP_SYMBOL(result)->remat &&
      ( OP_SYMBOL(result)->liveTo > ic->seq ||
        ic->depth )) {
      int size = AOP_SIZE(right) - 1;
      while (size--)
    emitcode("dec","%s",rname);
  }
    }

    /* done */
    freeAsmop(right,NULL,ic,TRUE);


}

/*-----------------------------------------------------------------*/
/* genFarPointerSet - set value from far space                     */
/*-----------------------------------------------------------------*/
static void genFarPointerSet (operand *right,
                              operand *result, iCode *ic)
{
    int size, offset ;
    sym_link *retype = getSpec(operandType(right));

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    aopOp(result,ic,FALSE);

    /* if the operand is already in dptr
    then we do nothing else we move the value to dptr */
    if (AOP_TYPE(result) != AOP_STR) {
        /* if this is remateriazable */
        if (AOP_TYPE(result) == AOP_IMMD)
            emitcode("mov","dptr,%s",aopGet(AOP(result),0,TRUE,FALSE));
        else { /* we need to get it byte by byte */
            emitcode("mov","dpl,%s",aopGet(AOP(result),0,FALSE,FALSE));
            emitcode("mov","dph,%s",aopGet(AOP(result),1,FALSE,FALSE));
            if (options.model == MODEL_FLAT24)
            {
               emitcode("mov", "dpx,%s",aopGet(AOP(result),2,FALSE,FALSE));
            }
        }
    }
    /* so dptr know contains the address */
    freeAsmop(result,NULL,ic,TRUE);
    aopOp(right,ic,FALSE);

    /* if bit then unpack */
    if (IS_BITVAR(retype))
        genPackBits(retype,right,"dptr",FPOINTER);
    else {
        size = AOP_SIZE(right);
        offset = 0 ;

        while (size--) {
            char *l = aopGet(AOP(right),offset++,FALSE,FALSE);
            MOVA(l);
            emitcode("movx","@dptr,a");
            if (size)
                emitcode("inc","dptr");
        }
    }

    freeAsmop(right,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genGenPointerSet - set value from generic pointer space         */
/*-----------------------------------------------------------------*/
static void genGenPointerSet (operand *right,
                              operand *result, iCode *ic)
{
    int size, offset ;
    sym_link *retype = getSpec(operandType(right));

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    aopOp(result,ic,FALSE);

    /* if the operand is already in dptr
    then we do nothing else we move the value to dptr */
    if (AOP_TYPE(result) != AOP_STR) {
        /* if this is remateriazable */
        if (AOP_TYPE(result) == AOP_IMMD) {
            emitcode("mov","dptr,%s",aopGet(AOP(result),0,TRUE,FALSE));
            emitcode("mov","b,%s + 1",aopGet(AOP(result),0,TRUE,FALSE));
        }
        else { /* we need to get it byte by byte */
    char *l = aopGet(AOP(result),0,FALSE,FALSE);
    if(strcmp("FSR",l))
      emitcode("movlw","%s",aopGet(AOP(result),0,FALSE,FALSE));

    emitcode("movwf","INDF");
        }
    }
    /* so dptr know contains the address */
    freeAsmop(result,NULL,ic,TRUE);
    aopOp(right,ic,FALSE);

    /* if bit then unpack */
    if (IS_BITVAR(retype))
        genPackBits(retype,right,"dptr",GPOINTER);
    else {
        size = AOP_SIZE(right);
        offset = 0 ;

        while (--size) {
            char *l = aopGet(AOP(right),offset++,FALSE,FALSE);
      if(size)
        emitcode("incf","fsr,f");
      emitcode("movf","%s,w",aopGet(AOP(right),offset++,FALSE,FALSE));
      emitcode("movwf","indf");
            //MOVA(l);
            //DEBUGemitcode(";lcall","__gptrput");
            //if (size)
            //    emitcode("inc","dptr");
        }
    }

    freeAsmop(right,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genPointerSet - stores the value into a pointer location        */
/*-----------------------------------------------------------------*/
static void genPointerSet (iCode *ic)
{
    operand *right, *result ;
    sym_link *type, *etype;
    int p_type;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    right = IC_RIGHT(ic);
    result = IC_RESULT(ic) ;

    /* depending on the type of pointer we need to
    move it to the correct pointer register */
    type = operandType(result);
    etype = getSpec(type);
    /* if left is of type of pointer then it is simple */
    if (IS_PTR(type) && !IS_FUNC(type->next)) {
        p_type = DCL_TYPE(type);
    }
    else {
  /* we have to go by the storage class */
  p_type = PTR_TYPE(SPEC_OCLS(etype));

/*  if (SPEC_OCLS(etype)->codesp ) { */
/*      p_type = CPOINTER ;  */
/*  } */
/*  else */
/*      if (SPEC_OCLS(etype)->fmap && !SPEC_OCLS(etype)->paged) */
/*    p_type = FPOINTER ; */
/*      else */
/*    if (SPEC_OCLS(etype)->fmap && SPEC_OCLS(etype)->paged) */
/*        p_type = PPOINTER ; */
/*    else */
/*        if (SPEC_OCLS(etype) == idata ) */
/*      p_type = IPOINTER ; */
/*        else */
/*      p_type = POINTER ; */
    }

    /* now that we have the pointer type we assign
    the pointer values */
    switch (p_type) {

    case POINTER:
    case IPOINTER:
  genNearPointerSet (right,result,ic);
  break;

    case PPOINTER:
  genPagedPointerSet (right,result,ic);
  break;

    case FPOINTER:
  genFarPointerSet (right,result,ic);
  break;

    case GPOINTER:
  genGenPointerSet (right,result,ic);
  break;
    }

}

/*-----------------------------------------------------------------*/
/* genIfx - generate code for Ifx statement                        */
/*-----------------------------------------------------------------*/
static void genIfx (iCode *ic, iCode *popIc)
{
    operand *cond = IC_COND(ic);
    int isbit =0;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);
    aopOp(cond,ic,FALSE);

    /* get the value into acc */
    if (AOP_TYPE(cond) != AOP_CRY)
        toBoolean(cond);
    else
        isbit = 1;
    /* the result is now in the accumulator */
    freeAsmop(cond,NULL,ic,TRUE);

    /* if there was something to be popped then do it */
    if (popIc)
        genIpop(popIc);

    /* if the condition is  a bit variable */
    if (isbit && IS_ITEMP(cond) &&
  SPIL_LOC(cond)) {
      genIfxJump(ic,SPIL_LOC(cond)->rname);
      DEBUGemitcode ("; isbit  SPIL_LOC","%s",SPIL_LOC(cond)->rname);
    }
    else {
      /*
  if (isbit && !IS_ITEMP(cond))
    DEBUGemitcode ("; isbit OP_SYM","%s",OP_SYMBOL(cond)->rname);
  else
    DEBUGemitcode ("; isbit","a");
      */

  if (isbit && !IS_ITEMP(cond))
      genIfxJump(ic,OP_SYMBOL(cond)->rname);
  else
      genIfxJump(ic,"a");
    }
    ic->generated = 1;
}

/*-----------------------------------------------------------------*/
/* genAddrOf - generates code for address of                       */
/*-----------------------------------------------------------------*/
static void genAddrOf (iCode *ic)
{
    symbol *sym = OP_SYMBOL(IC_LEFT(ic));
    int size, offset ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    aopOp(IC_RESULT(ic),ic,FALSE);

    /* if the operand is on the stack then we
    need to get the stack offset of this
    variable */
    if (sym->onStack) {
        /* if it has an offset then we need to compute
        it */
        if (sym->stack) {
            emitcode("mov","a,_bp");
            emitcode("add","a,#0x%02x",((char) sym->stack & 0xff));
            aopPut(AOP(IC_RESULT(ic)),"a",0);
        } else {
            /* we can just move _bp */
            aopPut(AOP(IC_RESULT(ic)),"_bp",0);
        }
        /* fill the result with zero */
        size = AOP_SIZE(IC_RESULT(ic)) - 1;


        if (options.stack10bit && size < (FPTRSIZE - 1))
        {
            fprintf(stderr,
                  "*** warning: pointer to stack var truncated.\n");
        }

        offset = 1;
        while (size--)
        {
            /* Yuck! */
            if (options.stack10bit && offset == 2)
            {
                aopPut(AOP(IC_RESULT(ic)),"#0x40", offset++);
            }
            else
            {
              aopPut(AOP(IC_RESULT(ic)),zero,offset++);
            }
        }

        goto release;
    }

    /* object not on stack then we need the name */
    size = AOP_SIZE(IC_RESULT(ic));
    offset = 0;

    while (size--) {
        char s[SDCC_NAME_MAX];
        if (offset)
            sprintf(s,"#(%s >> %d)",
                    sym->rname,
                    offset*8);
        else
            sprintf(s,"#%s",sym->rname);
        aopPut(AOP(IC_RESULT(ic)),s,offset++);
    }

release:
    freeAsmop(IC_RESULT(ic),NULL,ic,TRUE);

}

#if 0
/*-----------------------------------------------------------------*/
/* genFarFarAssign - assignment when both are in far space         */
/*-----------------------------------------------------------------*/
static void genFarFarAssign (operand *result, operand *right, iCode *ic)
{
    int size = AOP_SIZE(right);
    int offset = 0;
    char *l ;
    /* first push the right side on to the stack */
    while (size--) {
  l = aopGet(AOP(right),offset++,FALSE,FALSE);
  MOVA(l);
  emitcode ("push","acc");
    }

    freeAsmop(right,NULL,ic,FALSE);
    /* now assign DPTR to result */
    aopOp(result,ic,FALSE);
    size = AOP_SIZE(result);
    while (size--) {
  emitcode ("pop","acc");
  aopPut(AOP(result),"a",--offset);
    }
    freeAsmop(result,NULL,ic,FALSE);

}
#endif

/*-----------------------------------------------------------------*/
/* genAssign - generate code for assignment                        */
/*-----------------------------------------------------------------*/
static void genAssign (iCode *ic)
{
    operand *result, *right;
    int size, offset ;
  unsigned long lit = 0L;

    result = IC_RESULT(ic);
    right  = IC_RIGHT(ic) ;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    /* if they are the same */
    if (operandsEqu (IC_RESULT(ic),IC_RIGHT(ic)))
        return ;

    aopOp(right,ic,FALSE);
    aopOp(result,ic,TRUE);

    /* if they are the same registers */
    if (sameRegs(AOP(right),AOP(result)))
        goto release;

    /* if the result is a bit */
    if (AOP_TYPE(result) == AOP_CRY) {

        /* if the right size is a literal then
        we know what the value is */
        if (AOP_TYPE(right) == AOP_LIT) {
            if (((int) operandLitValue(right)))
        emitcode("bsf","(%s >> 3),(%s & 7)",
     AOP(result)->aopu.aop_dir,
     AOP(result)->aopu.aop_dir);
            else
        emitcode("bcf","(%s >> 3),(%s & 7)",
     AOP(result)->aopu.aop_dir,
     AOP(result)->aopu.aop_dir);
            goto release;
        }

        /* the right is also a bit variable */
        if (AOP_TYPE(right) == AOP_CRY) {
    emitcode("bcf","(%s >> 3),(%s & 7)",
       AOP(result)->aopu.aop_dir,
       AOP(result)->aopu.aop_dir);
    emitcode("btfsc","(%s >> 3),(%s & 7)",
       AOP(right)->aopu.aop_dir,
       AOP(right)->aopu.aop_dir);
    emitcode("bsf","(%s >> 3),(%s & 7)",
       AOP(result)->aopu.aop_dir,
       AOP(result)->aopu.aop_dir);
    goto release ;
        }

        /* we need to or */
        toBoolean(right);
        aopPut(AOP(result),"a",0);
        goto release ;
    }

    /* bit variables done */
    /* general case */
    size = AOP_SIZE(result);
    offset = 0 ;
    if(AOP_TYPE(right) == AOP_LIT)
  lit = (unsigned long)floatFromVal(AOP(right)->aopu.aop_lit);
    if((AOP_TYPE(result) != AOP_REG) &&
       (AOP_TYPE(right) == AOP_LIT) &&
       !IS_FLOAT(operandType(right)) &&
       (lit < 256L)){

  while (size--) {
      if((unsigned int)((lit >> (size*8)) & 0x0FFL)== 0)
        emitcode("clrf","%s", aopGet(AOP(result),size,FALSE,FALSE));
      else {
        emitcode("movlw","%s", aopGet(AOP(right),size,FALSE,FALSE));
        emitcode("movwf","%s", aopGet(AOP(result),size,FALSE,FALSE));
      }
  }
    } else {
  while (size--) {
    if(AOP_TYPE(right) == AOP_LIT)
      emitcode("movlw","%s", aopGet(AOP(right),offset,FALSE,FALSE));
    else
      emitcode("movf","%s,w", aopGet(AOP(right),offset,FALSE,FALSE));

    emitcode("movwf","%s", aopGet(AOP(result),offset,FALSE,FALSE));
    offset++;
  }
    }

release:
    freeAsmop (right,NULL,ic,FALSE);
    freeAsmop (result,NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genJumpTab - genrates code for jump table                       */
/*-----------------------------------------------------------------*/
static void genJumpTab (iCode *ic)
{
    symbol *jtab;
    char *l;

    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    aopOp(IC_JTCOND(ic),ic,FALSE);
    /* get the condition into accumulator */
    l = aopGet(AOP(IC_JTCOND(ic)),0,FALSE,FALSE);
    MOVA(l);
    /* multiply by three */
    emitcode("add","a,acc");
    emitcode("add","a,%s",aopGet(AOP(IC_JTCOND(ic)),0,FALSE,FALSE));
    freeAsmop(IC_JTCOND(ic),NULL,ic,TRUE);

    jtab = newiTempLabel(NULL);
    emitcode("mov","dptr,#%05d_DS_",jtab->key+100);
    emitcode("jmp","@a+dptr");
    emitcode("","%05d_DS_:",jtab->key+100);
    /* now generate the jump labels */
    for (jtab = setFirstItem(IC_JTLABELS(ic)) ; jtab;
         jtab = setNextItem(IC_JTLABELS(ic)))
        emitcode("ljmp","%05d_DS_",jtab->key+100);

}

/*-----------------------------------------------------------------*/
/* genMixedOperation - gen code for operators between mixed types  */
/*-----------------------------------------------------------------*/
/*
  TSD - Written for the PIC port - but this unfortunately is buggy.
  This routine is good in that it is able to efficiently promote
  types to different (larger) sizes. Unfortunately, the temporary
  variables that are optimized out by this routine are sometimes
  used in other places. So until I know how to really parse the
  iCode tree, I'm going to not be using this routine :(.
*/
static int genMixedOperation (iCode *ic)
{
#if 0
  operand *result = IC_RESULT(ic);
  sym_link *ctype = operandType(IC_LEFT(ic));
  operand *right = IC_RIGHT(ic);
  int ret = 0;
  int big,small;
  int offset;

  iCode *nextic;
  operand *nextright=NULL,*nextleft=NULL,*nextresult=NULL;

  emitcode("; ***","%s  %d",__FUNCTION__,__LINE__);

  nextic = ic->next;
  if(!nextic)
    return 0;

  nextright = IC_RIGHT(nextic);
  nextleft  = IC_LEFT(nextic);
  nextresult = IC_RESULT(nextic);

  aopOp(right,ic,FALSE);
  aopOp(result,ic,FALSE);
  aopOp(nextright,  nextic, FALSE);
  aopOp(nextleft,   nextic, FALSE);
  aopOp(nextresult, nextic, FALSE);

  if (sameRegs(AOP(IC_RESULT(ic)), AOP(IC_RIGHT(nextic)))) {

    operand *t = right;
    right = nextright;
    nextright = t;

    emitcode(";remove right +","");

  } else   if (sameRegs(AOP(IC_RESULT(ic)), AOP(IC_LEFT(nextic)))) {
/*
    operand *t = right;
    right = nextleft;
    nextleft = t;
*/
    emitcode(";remove left +","");
  } else
    return 0;

  big = AOP_SIZE(nextleft);
  small = AOP_SIZE(nextright);

  switch(nextic->op) {

  case '+':
    emitcode(";optimize a +","");
    /* if unsigned or not an integral type */
    if (AOP_TYPE(IC_LEFT(nextic)) == AOP_CRY) {
      emitcode(";add a bit to something","");
    } else {

      emitcode("movf","%s,w",AOP(nextright)->aopu.aop_dir);

      if (!sameRegs(AOP(IC_LEFT(nextic)), AOP(IC_RESULT(nextic))) ) {
  emitcode("addwf","%s,w",AOP(nextleft)->aopu.aop_dir);
  emitcode("movwf","%s",aopGet(AOP(IC_RESULT(nextic)),0,FALSE,FALSE));
      } else
  emitcode("addwf","%s,f",AOP(nextleft)->aopu.aop_dir);

      offset = 0;
      while(--big) {

  offset++;

  if(--small) {
    if (!sameRegs(AOP(IC_LEFT(nextic)), AOP(IC_RESULT(nextic))) ){
      emitcode("movf","%s,w",aopGet(AOP(IC_LEFT(nextic)),offset,FALSE,FALSE));
      emitcode("movwf","%s,f",aopGet(AOP(IC_RESULT(nextic)),offset,FALSE,FALSE) );
    }

    emitcode("movf","%s,w", aopGet(AOP(IC_LEFT(nextic)),offset,FALSE,FALSE));
    emitSKPNC;
    emitcode("btfsc","(%s >> 3), (%s & 7)",
       AOP(IC_RIGHT(nextic))->aopu.aop_dir,
       AOP(IC_RIGHT(nextic))->aopu.aop_dir);
    emitcode(" incf","%s,w", aopGet(AOP(IC_LEFT(nextic)),offset,FALSE,FALSE));
    emitcode("movwf","%s", aopGet(AOP(IC_RESULT(nextic)),offset,FALSE,FALSE));

  } else {
    emitcode("rlf","known_zero,w");

    /*
      if right is signed
        btfsc  right,7
               addlw ff
    */
    if (!sameRegs(AOP(IC_LEFT(nextic)), AOP(IC_RESULT(nextic))) ){
      emitcode("addwf","%s,w",aopGet(AOP(IC_LEFT(nextic)),offset,FALSE,FALSE));
      emitcode("movwf","%s,f",aopGet(AOP(IC_RESULT(nextic)),offset,FALSE,FALSE) );
    } else {
      emitcode("addwf","%s,f",aopGet(AOP(IC_RESULT(nextic)),offset,FALSE,FALSE) );
    }
  }
      }
      ret = 1;
    }
  }
  ret = 1;

release:
  freeAsmop(right,NULL,ic,TRUE);
  freeAsmop(result,NULL,ic,TRUE);
  freeAsmop(nextright,NULL,ic,TRUE);
  freeAsmop(nextleft,NULL,ic,TRUE);
  if(ret)
    nextic->generated = 1;

  return ret;
#else
  return 0;
#endif
}
/*-----------------------------------------------------------------*/
/* genCast - gen code for casting                                  */
/*-----------------------------------------------------------------*/
static void genCast (iCode *ic)
{
    operand *result = IC_RESULT(ic);
    sym_link *ctype = operandType(IC_LEFT(ic));
    operand *right = IC_RIGHT(ic);
    int size, offset ;

    emitcode("; ***","%s  %d",__FUNCTION__,__LINE__);
    /* if they are equivalent then do nothing */
    if (operandsEqu(IC_RESULT(ic),IC_RIGHT(ic)))
        return ;

    aopOp(right,ic,FALSE) ;
    aopOp(result,ic,FALSE);

    /* if the result is a bit */
    if (AOP_TYPE(result) == AOP_CRY) {
        /* if the right size is a literal then
        we know what the value is */
        if (AOP_TYPE(right) == AOP_LIT) {
    emitcode("; ***  right is a lit","%s  %d",__FUNCTION__,__LINE__);
            if (((int) operandLitValue(right)))
        emitcode("bsf","(%s >> 3), (%s & 7)",
           AOP(result)->aopu.aop_dir,
           AOP(result)->aopu.aop_dir);
            else
        emitcode("bcf","(%s >> 3), (%s & 7)",
           AOP(result)->aopu.aop_dir,
           AOP(result)->aopu.aop_dir);

            goto release;
        }

        /* the right is also a bit variable */
        if (AOP_TYPE(right) == AOP_CRY) {
    emitcode("clrc","");
    emitcode("btfsc","(%s >> 3), (%s & 7)",
       AOP(right)->aopu.aop_dir,
       AOP(right)->aopu.aop_dir);
            aopPut(AOP(result),"c",0);
            goto release ;
        }

        /* we need to or */
        toBoolean(right);
        aopPut(AOP(result),"a",0);
        goto release ;
    }

    /* if they are the same size : or less */
    if (AOP_SIZE(result) <= AOP_SIZE(right)) {

        /* if they are in the same place */
        if (sameRegs(AOP(right),AOP(result)))
            goto release;

        /* if they in different places then copy */
        size = AOP_SIZE(result);
        offset = 0 ;
        while (size--) {
            aopPut(AOP(result),
                   aopGet(AOP(right),offset,FALSE,FALSE),
                   offset);
            offset++;
        }
        goto release;
    }


    /* if the result is of type pointer */
    if (IS_PTR(ctype)) {

  int p_type;
  sym_link *type = operandType(right);
  sym_link *etype = getSpec(type);

  /* pointer to generic pointer */
  if (IS_GENPTR(ctype)) {
      char *l = zero;

      if (IS_PTR(type))
    p_type = DCL_TYPE(type);
      else {
    /* we have to go by the storage class */
    p_type = PTR_TYPE(SPEC_OCLS(etype));

/*    if (SPEC_OCLS(etype)->codesp )  */
/*        p_type = CPOINTER ;  */
/*    else */
/*        if (SPEC_OCLS(etype)->fmap && !SPEC_OCLS(etype)->paged) */
/*      p_type = FPOINTER ; */
/*        else */
/*      if (SPEC_OCLS(etype)->fmap && SPEC_OCLS(etype)->paged) */
/*          p_type = PPOINTER; */
/*      else */
/*          if (SPEC_OCLS(etype) == idata ) */
/*        p_type = IPOINTER ; */
/*          else */
/*        p_type = POINTER ; */
      }

      /* the first two bytes are known */
      size = GPTRSIZE - 1;
      offset = 0 ;
      while (size--) {
    aopPut(AOP(result),
           aopGet(AOP(right),offset,FALSE,FALSE),
           offset);
    offset++;
      }
      /* the last byte depending on type */
      switch (p_type) {
      case IPOINTER:
      case POINTER:
    l = zero;
    break;
      case FPOINTER:
    l = one;
    break;
      case CPOINTER:
    l = "#0x02";
    break;
      case PPOINTER:
    l = "#0x03";
    break;

      default:
    /* this should never happen */
    werror(E_INTERNAL_ERROR,__FILE__,__LINE__,
           "got unknown pointer type");
    exit(1);      }
      aopPut(AOP(result),l, GPTRSIZE - 1);
      goto release ;
  }

  /* just copy the pointers */
  size = AOP_SIZE(result);
  offset = 0 ;
  while (size--) {
      aopPut(AOP(result),
       aopGet(AOP(right),offset,FALSE,FALSE),
       offset);
      offset++;
  }
  goto release ;
    }


    if (AOP_TYPE(right) == AOP_CRY) {
      int offset = 1;
      size = AOP_SIZE(right);

      emitcode("clrf","%s ; %d", aopGet(AOP(result),0,FALSE,FALSE),__LINE__);
      emitcode("btfsc","(%s >> 3), (%s & 7)",
         AOP(right)->aopu.aop_dir,
         AOP(right)->aopu.aop_dir);
      emitcode("incf","%s,f", aopGet(AOP(result),0,FALSE,FALSE),__LINE__);
      while (size--) {
  emitcode("clrf","%s", aopGet(AOP(result),offset++,FALSE,FALSE),__LINE__);
      }
      goto release;
    }

    /* so we now know that the size of destination is greater
    than the size of the source.
    Now, if the next iCode is an operator then we might be
    able to optimize the operation without performing a cast.
    */
    if(genMixedOperation(ic))
      goto release;


    /* we move to result for the size of source */
    size = AOP_SIZE(right);
    offset = 0 ;
    while (size--) {
      emitcode(";","%d",__LINE__);
        aopPut(AOP(result),
               aopGet(AOP(right),offset,FALSE,FALSE),
               offset);
        offset++;
    }

    /* now depending on the sign of the destination */
    size = AOP_SIZE(result) - AOP_SIZE(right);
    /* if unsigned or not an integral type */
    if (SPEC_USIGN(ctype) || !IS_SPEC(ctype)) {
        while (size--)
    emitcode("clrf","%s",aopGet(AOP(result),offset++,FALSE,FALSE));
    } else {
      /* we need to extend the sign :{ */
      //char *l = aopGet(AOP(right),AOP_SIZE(right) - 1,FALSE,FALSE);
      //MOVA(l);

        emitcode("clrw","");
  emitcode("btfsc","(%s >> 3), (%s & 7)",
     AOP(right)->aopu.aop_dir,
     AOP(right)->aopu.aop_dir);
        emitcode("movlw","0xff");
        while (size--) {
    emitcode("movwf","%s",aopGet(AOP(result),offset++,FALSE,FALSE));
    // aopPut(AOP(result),"a",offset++);
  }

    }

    /* we are done hurray !!!! */

release:
    freeAsmop(right,NULL,ic,TRUE);
    freeAsmop(result,NULL,ic,TRUE);

}

/*-----------------------------------------------------------------*/
/* genDjnz - generate decrement & jump if not zero instrucion      */
/*-----------------------------------------------------------------*/
static int genDjnz (iCode *ic, iCode *ifx)
{
    symbol *lbl, *lbl1;
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    if (!ifx)
  return 0;

    /* if the if condition has a false label
       then we cannot save */
    if (IC_FALSE(ifx))
  return 0;

    /* if the minus is not of the form
       a = a - 1 */
    if (!isOperandEqual(IC_RESULT(ic),IC_LEFT(ic)) ||
  !IS_OP_LITERAL(IC_RIGHT(ic)))
  return 0;

    if (operandLitValue(IC_RIGHT(ic)) != 1)
  return 0;

    /* if the size of this greater than one then no
       saving */
    if (getSize(operandType(IC_RESULT(ic))) > 1)
  return 0;

    /* otherwise we can save BIG */
    lbl = newiTempLabel(NULL);
    lbl1= newiTempLabel(NULL);

    aopOp(IC_RESULT(ic),ic,FALSE);

    if (IS_AOP_PREG(IC_RESULT(ic))) {
  emitcode("dec","%s",
     aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));
  emitcode("mov","a,%s",aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));
  emitcode("jnz","%05d_DS_",lbl->key+100);
    } else {
      emitcode("decfsz","%s,f",aopGet(AOP(IC_RESULT(ic)),0,FALSE,FALSE));
      emitcode ("goto","_%05d_DS_",IC_TRUE(ifx)->key+100 + labelOffset);

    }
/*     emitcode ("sjmp","%05d_DS_",lbl1->key+100); */
/*     emitcode ("","%05d_DS_:",lbl->key+100); */
/*     emitcode ("ljmp","%05d_DS_",IC_TRUE(ifx)->key+100); */
/*     emitcode ("","%05d_DS_:",lbl1->key+100); */


    freeAsmop(IC_RESULT(ic),NULL,ic,TRUE);
    ifx->generated = 1;
    return 1;
}

/*-----------------------------------------------------------------*/
/* genReceive - generate code for a receive iCode                  */
/*-----------------------------------------------------------------*/
static void genReceive (iCode *ic)
{
    DEBUGemitcode ("; ***","%s  %d",__FUNCTION__,__LINE__);

    if (isOperandInFarSpace(IC_RESULT(ic)) &&
  ( OP_SYMBOL(IC_RESULT(ic))->isspilt ||
    IS_TRUE_SYMOP(IC_RESULT(ic))) ) {

  int size = getSize(operandType(IC_RESULT(ic)));
  int offset =  fReturnSize - size;
  while (size--) {
      emitcode ("push","%s", (strcmp(fReturn[fReturnSize - offset - 1],"a") ?
            fReturn[fReturnSize - offset - 1] : "acc"));
      offset++;
  }
  aopOp(IC_RESULT(ic),ic,FALSE);
  size = AOP_SIZE(IC_RESULT(ic));
  offset = 0;
  while (size--) {
      emitcode ("pop","acc");
      aopPut (AOP(IC_RESULT(ic)),"a",offset++);
  }

    } else {
  _G.accInUse++;
  aopOp(IC_RESULT(ic),ic,FALSE);
  _G.accInUse--;
  assignResultValue(IC_RESULT(ic));
    }

    freeAsmop(IC_RESULT(ic),NULL,ic,TRUE);
}

/*-----------------------------------------------------------------*/
/* genpic14Code - generate code for pic14 based controllers            */
/*-----------------------------------------------------------------*/
void genpic14Code (iCode *lic)
{
    iCode *ic;
    int cln = 0;

    lineHead = lineCurr = NULL;

    /* if debug information required */
/*     if (options.debug && currFunc) { */
    if (currFunc) {
  cdbSymbol(currFunc,cdbFile,FALSE,TRUE);
  _G.debugLine = 1;
  if (IS_STATIC(currFunc->etype))
      emitcode("",";F%s$%s$0$0     %d",moduleName,currFunc->name,__LINE__);
  else
      emitcode("",";G$%s$0$0   %d",currFunc->name,__LINE__);
  _G.debugLine = 0;
    }


    for (ic = lic ; ic ; ic = ic->next ) {

      DEBUGemitcode(";ic","");
  if ( cln != ic->lineno ) {
      if ( options.debug ) {
    _G.debugLine = 1;
    emitcode("",";C$%s$%d$%d$%d ==.",
       ic->filename,ic->lineno,
       ic->level,ic->block);
    _G.debugLine = 0;
      }
      emitcode(";","%s %d",ic->filename,ic->lineno);
      cln = ic->lineno ;
  }
  /* if the result is marked as
     spilt and rematerializable or code for
     this has already been generated then
     do nothing */
  if (resultRemat(ic) || ic->generated )
      continue ;

  /* depending on the operation */
  switch (ic->op) {
  case '!' :
      genNot(ic);
      break;

  case '~' :
      genCpl(ic);
      break;

  case UNARYMINUS:
      genUminus (ic);
      break;

  case IPUSH:
      genIpush (ic);
      break;

  case IPOP:
      /* IPOP happens only when trying to restore a
         spilt live range, if there is an ifx statement
         following this pop then the if statement might
         be using some of the registers being popped which
         would destory the contents of the register so
         we need to check for this condition and handle it */
      if (ic->next            &&
    ic->next->op == IFX &&
    regsInCommon(IC_LEFT(ic),IC_COND(ic->next)))
    genIfx (ic->next,ic);
      else
    genIpop (ic);
      break;

  case CALL:
      genCall (ic);
      break;

  case PCALL:
      genPcall (ic);
      break;

  case FUNCTION:
      genFunction (ic);
      break;

  case ENDFUNCTION:
      genEndFunction (ic);
      break;

  case RETURN:
      genRet (ic);
      break;

  case LABEL:
      genLabel (ic);
      break;

  case GOTO:
      genGoto (ic);
      break;

  case '+' :
      genPlus (ic) ;
      break;

  case '-' :
      if ( ! genDjnz (ic,ifxForOp(IC_RESULT(ic),ic)))
    genMinus (ic);
      break;

  case '*' :
      genMult (ic);
      break;

  case '/' :
      genDiv (ic) ;
      break;

  case '%' :
      genMod (ic);
      break;

  case '>' :
      genCmpGt (ic,ifxForOp(IC_RESULT(ic),ic));
      break;

  case '<' :
      genCmpLt (ic,ifxForOp(IC_RESULT(ic),ic));
      break;

  case LE_OP:
  case GE_OP:
  case NE_OP:

      /* note these two are xlated by algebraic equivalence
         during parsing SDCC.y */
      werror(E_INTERNAL_ERROR,__FILE__,__LINE__,
       "got '>=' or '<=' shouldn't have come here");
      break;

  case EQ_OP:
      genCmpEq (ic,ifxForOp(IC_RESULT(ic),ic));
      break;

  case AND_OP:
      genAndOp (ic);
      break;

  case OR_OP:
      genOrOp (ic);
      break;

  case '^' :
      genXor (ic,ifxForOp(IC_RESULT(ic),ic));
      break;

  case '|' :
    genOr (ic,ifxForOp(IC_RESULT(ic),ic));
      break;

  case BITWISEAND:
            genAnd (ic,ifxForOp(IC_RESULT(ic),ic));
      break;

  case INLINEASM:
      genInline (ic);
      break;

  case RRC:
      genRRC (ic);
      break;

  case RLC:
      genRLC (ic);
      break;

  case GETHBIT:
      genGetHbit (ic);
      break;

  case LEFT_OP:
      genLeftShift (ic);
      break;

  case RIGHT_OP:
      genRightShift (ic);
      break;

  case GET_VALUE_AT_ADDRESS:
      genPointerGet(ic);
      break;

  case '=' :
      if (POINTER_SET(ic))
    genPointerSet(ic);
      else
    genAssign(ic);
      break;

  case IFX:
      genIfx (ic,NULL);
      break;

  case ADDRESS_OF:
      genAddrOf (ic);
      break;

  case JUMPTABLE:
      genJumpTab (ic);
      break;

  case CAST:
      genCast (ic);
      break;

  case RECEIVE:
      genReceive(ic);
      break;

  case SEND:
      addSet(&_G.sendSet,ic);
      break;

  default :
      ic = ic;
      /*      piCode(ic,stdout); */

        }
    }


    /* now we are ready to call the
       peep hole optimizer */
    if (!options.nopeep) {
      printf("peep hole optimizing\n");
  peepHole (&lineHead);
    }
    /* now do the actual printing */
    printLine (lineHead,codeOutFile);
    return;
}
