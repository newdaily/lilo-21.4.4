LATEX=latex
RLATEX=./rlatex
FIG2DEV=fig2dev
SHELL=/bin/sh

IMAGES=bootloader.tex map.tex image.tex parameter.tex other.tex

.SUFFIXES:	.fig .tex .dvi .ps

.fig.tex:
		$(FIG2DEV) -L latex $*.fig $*.tex

.tex.dvi:
		$(RLATEX) $*

all:		user.dvi tech.dvi

user.ps:	user.dvi
		dvips -o user.ps user.dvi

tech.ps:	tech.dvi
		dvips -o tech.ps tech.dvi

user.dvi:	user.tex

tech.dvi:	tech.tex $(IMAGES)

../README:	user.tex
		perl t2a.pl <user.tex >../README || { rm -f ../README; exit 1; }

again:
		$(LATEX) user
		$(LATEX) tech

clean:
		rm -f *.toc *.aux *.log

spotless:	clean
		rm -f *.dvi $(IMAGES)

dummies:
		for n in $(IMAGES); do echo '(omitted)' >$$n; done
