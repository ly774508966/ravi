/*
** LuaJIT VM builder.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
**
** This is a tool to build the hand-tuned assembler code required for
** LuaJIT's bytecode interpreter. It supports a variety of output formats
** to feed different toolchains (see usage() below).
**
** This tool is not particularly optimized because it's only used while
** _building_ LuaJIT. There's no point in distributing or installing it.
** Only the object code generated by this tool is linked into LuaJIT.
**
** Caveat: some memory is not free'd, error handling is lazy.
** It's a one-shot tool -- any effort fixing this would be wasted.
*/

#include "buildvm.h"
#include "lua.h"
#include "lua.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

/* ------------------------------------------------------------------------ */

/* DynASM glue definitions. */
#define Dst		ctx
#define Dst_DECL	BuildCtx *ctx
#define Dst_REF		(ctx->D)
#define DASM_CHECKS	1

#include "../dynasm/dasm_proto.h"

/* Glue macros for DynASM. */
static int collect_reloc(BuildCtx *ctx, uint8_t *addr, int idx, int type);

#define DASM_EXTERN(ctx, addr, idx, type) \
  collect_reloc(ctx, addr, idx, type)

/* ------------------------------------------------------------------------ */

/* Avoid trouble if cross-compiling for an x86 target. Speed doesn't matter. */
#define DASM_ALIGNED_WRITES	1

/* Embed architecture-specific DynASM encoder. */
#if RAVI_TARGET_X86ORX64
#include "../dynasm/dasm_x86.h"
#elif RAVI_TARGET_ARM
#include "../dynasm/dasm_arm.h"
#elif RAVI_TARGET_ARM64
#include "../dynasm/dasm_arm64.h"
#elif RAVI_TARGET_PPC
#include "../dynasm/dasm_ppc.h"
#elif RAVI_TARGET_MIPS
#include "../dynasm/dasm_mips.h"
#else
#error "No support for this architecture (yet)"
#endif

/* Embed generated architecture-specific backend. */
#include "buildvm_arch.h"

/* ------------------------------------------------------------------------ */

void owrite(BuildCtx *ctx, const void *ptr, size_t sz)
{
  if (fwrite(ptr, 1, sz, ctx->fp) != sz) {
    fprintf(stderr, "Error: cannot write to output file: %s\n",
	    strerror(errno));
    exit(1);
  }
}

/* ------------------------------------------------------------------------ */

/* Emit code as raw bytes. Only used for DynASM debugging. */
static void emit_raw(BuildCtx *ctx)
{
  owrite(ctx, ctx->code, ctx->codesz);
}

/* -- Build machine code -------------------------------------------------- */

static const char *sym_decorate(BuildCtx *ctx,
				const char *prefix, const char *suffix)
{
  char name[256];
  char *p;
#if RAVI_64
  const char *symprefix = ctx->mode == BUILD_machasm ? "_" : "";
#else
  const char *symprefix = ctx->mode != BUILD_elfasm ? "_" : "";
#endif
  sprintf(name, "%s%s%s", symprefix, prefix, suffix);
  p = strchr(name, '@');
  if (p) {
#if RAVI_TARGET_X86ORX64
    if (!RAVI_64 && (ctx->mode == BUILD_coffasm || ctx->mode == BUILD_peobj))
      name[0] = name[1] == 'R' ? '_' : '@';  /* Just for _RtlUnwind@16. */
    else
      *p = '\0';
#else
    *p = '\0';
#endif
  }
  p = (char *)malloc(strlen(name)+1);  /* MSVC doesn't like strdup. */
  strcpy(p, name);
  return p;
}

#define NRELOCSYM	(sizeof(extnames)/sizeof(extnames[0])-1)

static int relocmap[NRELOCSYM+1]; // Dibyendu: add +1 to allow no extnames

/* Collect external relocations. */
static int collect_reloc(BuildCtx *ctx, uint8_t *addr, int idx, int type)
{
  if (ctx->nreloc >= BUILD_MAX_RELOC) {
    fprintf(stderr, "Error: too many relocations, increase BUILD_MAX_RELOC.\n");
    exit(1);
  }
  if (relocmap[idx] < 0) {
    relocmap[idx] = ctx->nrelocsym;
    ctx->relocsym[ctx->nrelocsym] = sym_decorate(ctx, "", extnames[idx]);
    ctx->nrelocsym++;
  }
  ctx->reloc[ctx->nreloc].ofs = (int32_t)(addr - ctx->code);
  ctx->reloc[ctx->nreloc].sym = relocmap[idx];
  ctx->reloc[ctx->nreloc].type = type;
  ctx->nreloc++;
  return 0;  /* Encode symbol offset of 0. */
}

/* Naive insertion sort. Performance doesn't matter here. */
static void sym_insert(BuildCtx *ctx, int32_t ofs,
		       const char *prefix, const char *suffix)
{
  ptrdiff_t i = ctx->nsym++;
  while (i > 0) {
    if (ctx->sym[i-1].ofs <= ofs)
      break;
    ctx->sym[i] = ctx->sym[i-1];
    i--;
  }
  ctx->sym[i].ofs = ofs;
  ctx->sym[i].name = sym_decorate(ctx, prefix, suffix);
}

/* Build the machine code. */
static int build_code(BuildCtx *ctx)
{
  int status;
  int i;

  /* Initialize DynASM structures. */
  ctx->nglob = GLOB__MAX;
  ctx->glob = (void **)malloc(ctx->nglob*sizeof(void *));
  memset(ctx->glob, 0, ctx->nglob*sizeof(void *));
  ctx->nreloc = 0;

  ctx->globnames = globnames;
  ctx->extnames = extnames;
  ctx->relocsym = (const char **)malloc(NRELOCSYM*sizeof(const char *));
  ctx->nrelocsym = 0;
  for (i = 0; i < (int)NRELOCSYM; i++) relocmap[i] = -1;

  ctx->dasm_ident = DASM_IDENT;
  ctx->dasm_arch = DASM_ARCH;

  dasm_init(Dst, DASM_MAXSECTION);
  dasm_setupglobal(Dst, ctx->glob, ctx->nglob);
  dasm_setup(Dst, build_actionlist);

  /* Call arch-specific backend to emit the code. */
  ctx->npc = build_backend(ctx);

  /* Finalize the code. */
  (void)dasm_checkstep(Dst, -1);
  if ((status = dasm_link(Dst, &ctx->codesz))) return status;
  ctx->code = (uint8_t *)malloc(ctx->codesz);
  if ((status = dasm_encode(Dst, (void *)ctx->code))) return status;

  /* Allocate symbol table and bytecode offsets. */
  ctx->beginsym = sym_decorate(ctx, "", LABEL_PREFIX "vm_asm_begin");
  ctx->sym = (BuildSym *)malloc((ctx->npc+ctx->nglob+1)*sizeof(BuildSym));
  ctx->nsym = 0;
  ctx->bc_ofs = (int32_t *)malloc(ctx->npc*sizeof(int32_t));

  /* Collect the opcodes (PC labels). */
  for (i = 0; i < ctx->npc; i++) {
    int32_t ofs = dasm_getpclabel(Dst, i);
    if (ofs < 0) return 0x22000000|i;
    ctx->bc_ofs[i] = ofs;
// Dibyendu: Not sure what this is
//    if ((RAVI_HASJIT ||
//	 !(i == BC_JFORI || i == BC_JFORL || i == BC_JITERL || i == BC_JLOOP ||
//	   i == BC_IFORL || i == BC_IITERL || i == BC_ILOOP)) &&
//	(RAVI_HASFFI || i != BC_KCDATA))
//      sym_insert(ctx, ofs, LABEL_PREFIX_BC, bc_names[i]);
  }

  /* Collect the globals (named labels). */
  for (i = 0; i < ctx->nglob; i++) {
    const char *gl = globnames[i];
    int len = (int)strlen(gl);
    if (!ctx->glob[i]) {
      fprintf(stderr, "Error: undefined global %s\n", gl);
      exit(2);
    }
    /* Skip the _Z symbols. */
    if (!(len >= 2 && gl[len-2] == '_' && gl[len-1] == 'Z'))
      sym_insert(ctx, (int32_t)((uint8_t *)(ctx->glob[i]) - ctx->code),
		 LABEL_PREFIX, globnames[i]);
  }

  /* Close the address range. */
  sym_insert(ctx, (int32_t)ctx->codesz, "", "");
  ctx->nsym--;

  dasm_free(Dst);

  return 0;
}

static const char *lower(char *buf, const char *s)
{
  char *p = buf;
  while (*s) {
    *p++ = (*s >= 'A' && *s <= 'Z') ? *s+0x20 : *s;
    s++;
  }
  *p = '\0';
  return buf;
}

/* Emit C source code for bytecode-related definitions. */
static void emit_bcdef(BuildCtx *ctx)
{
  int i;
  fprintf(ctx->fp, "/* This is a generated file. DO NOT EDIT! */\n\n");
  fprintf(ctx->fp, "RAVI_DATADEF const uint16_t lj_bc_ofs[] = {\n");
  for (i = 0; i < ctx->npc; i++) {
    if (i != 0)
      fprintf(ctx->fp, ",\n");
    fprintf(ctx->fp, "%d", ctx->bc_ofs[i]);
  }
}

/* -- Argument parsing ---------------------------------------------------- */

/* Build mode names. */
static const char *const modenames[] = {
#define BUILDNAME(name)		#name,
BUILDDEF(BUILDNAME)
#undef BUILDNAME
  NULL
};

#define LUAJIT_VERSION    "LuaJIT 2.1.0-beta3"
#define LUAJIT_COPYRIGHT  "Copyright (C) 2005-2017 Mike Pall"
#define LUAJIT_URL    "http://luajit.org/"

/* Print usage information and exit. */
static void usage(void)
{
  int i;
  fprintf(stderr, LUAJIT_VERSION " VM builder.\n");
  fprintf(stderr, LUAJIT_COPYRIGHT ", " LUAJIT_URL "\n");
  fprintf(stderr, "Target architecture: " RAVI_ARCH_NAME "\n\n");
  fprintf(stderr, "Usage: buildvm -m mode [-o outfile] [infiles...]\n\n");
  fprintf(stderr, "Available modes:\n");
  for (i = 0; i < BUILD__MAX; i++)
    fprintf(stderr, "  %s\n", modenames[i]);  
  exit(1);
}

/* Parse the output mode name. */
static BuildMode parsemode(const char *mode)
{
  int i;
  for (i = 0; modenames[i]; i++)
    if (!strcmp(mode, modenames[i]))
      return (BuildMode)i;
  usage();
  return (BuildMode)-1;
}

/* Parse arguments. */
static void parseargs(BuildCtx *ctx, char **argv)
{
  const char *a;
  int i;
  ctx->mode = (BuildMode)-1;
  ctx->outname = "-";
  for (i = 1; (a = argv[i]) != NULL; i++) {
    if (a[0] != '-')
      break;
    switch (a[1]) {
    case '-':
      if (a[2]) goto err;
      i++;
      goto ok;
    case '\0':
      goto ok;
    case 'm':
      i++;
      if (a[2] || argv[i] == NULL) goto err;
      ctx->mode = parsemode(argv[i]);
      break;
    case 'o':
      i++;
      if (a[2] || argv[i] == NULL) goto err;
      ctx->outname = argv[i];
      break;
    default: err:
      usage();
      break;
    }
  }
ok:
  ctx->args = argv+i;
  if (ctx->mode == (BuildMode)-1) goto err;
}

int main(int argc, char **argv)
{
  BuildCtx ctx_;
  BuildCtx *ctx = &ctx_;
  int status, binmode;

  if (sizeof(void *) != 4*RAVI_32+8*RAVI_64) {
    fprintf(stderr,"Error: pointer size mismatch in cross-build.\n");
    fprintf(stderr,"Try: make HOST_CC=\"gcc -m32\" CROSS=...\n\n");
    return 1;
  }

  UNUSED(argc);
  parseargs(ctx, argv);

  if ((status = build_code(ctx))) {
    fprintf(stderr,"Error: DASM error %08x\n", status);
    return 1;
  }

  switch (ctx->mode) {
  case BUILD_peobj:
  case BUILD_raw:
    binmode = 1;
    break;
  default:
    binmode = 0;
    break;
  }

  if (ctx->outname[0] == '-' && ctx->outname[1] == '\0') {
    ctx->fp = stdout;
#if defined(_WIN32)
    if (binmode)
      _setmode(_fileno(stdout), _O_BINARY);  /* Yuck. */
#endif
  } else if (!(ctx->fp = fopen(ctx->outname, binmode ? "wb" : "w"))) {
    fprintf(stderr, "Error: cannot open output file '%s': %s\n",
	    ctx->outname, strerror(errno));
    exit(1);
  }

  switch (ctx->mode) {
  case BUILD_elfasm:
  case BUILD_coffasm:
  case BUILD_machasm:
    emit_asm(ctx);
    //emit_asm_debug(ctx);
    break;
  case BUILD_peobj:
    emit_peobj(ctx);
    break;
  case BUILD_raw:
    emit_raw(ctx);
    break;
  case BUILD_bcdef:
    emit_bcdef(ctx);
    break;
  default:
    break;
  }

  fflush(ctx->fp);
  if (ferror(ctx->fp)) {
    fprintf(stderr, "Error: cannot write to output file: %s\n",
	    strerror(errno));
    exit(1);
  }
  fclose(ctx->fp);

  return 0;
}

