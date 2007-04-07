#include <u.h>
#include <libc.h>
#include <ip.h>
#include "dns.h"

static RR*	doextquery(DNSmsg*, Request*, int);
static void	hint(RR**, RR*);

/* set in dns.c */
int	norecursion;		/* don't allow recursive requests */

/*
 *  answer a dns request
 */
void
dnserver(DNSmsg *reqp, DNSmsg *repp, Request *req, uchar *srcip, int rcode)
{
	int recursionflag;
	char *cp, *errmsg;
	char tname[32];
	DN *nsdp, *dp;
	Area *myarea;
	RR *tp, *neg;

	dncheck(nil, 1);

	recursionflag = norecursion? 0: Fcanrec;
	memset(repp, 0, sizeof(*repp));
	repp->id = reqp->id;
	repp->flags = Fresp | recursionflag | Oquery;

	/* move one question from reqp to repp */
	tp = reqp->qd;
	reqp->qd = tp->next;
	tp->next = nil;
	repp->qd = tp;

	if (rcode) {
		errmsg = "";
		if (rcode >= 0 && rcode < nrname)
			errmsg = rname[rcode];
		dnslog("server: response code 0%o (%s), req from %I",
			rcode, errmsg, srcip);
		/* provide feedback to clients who send us trash */
		repp->flags = (rcode&Rmask) | Fresp | Fcanrec | Oquery;
		return;
	}
	if(!rrsupported(repp->qd->type)){
		dnslog("server: unsupported request %s from %I",
			rrname(repp->qd->type, tname, sizeof tname), srcip);
		repp->flags = Runimplimented | Fresp | Fcanrec | Oquery;
		return;
	}

	if(repp->qd->owner->class != Cin){
		dnslog("server: unsupported class %d from %I",
			repp->qd->owner->class, srcip);
		repp->flags = Runimplimented | Fresp | Fcanrec | Oquery;
		return;
	}

	myarea = inmyarea(repp->qd->owner->name);
	if(myarea != nil) {
		if(repp->qd->type == Tixfr || repp->qd->type == Taxfr){
			dnslog("server: unsupported xfr request %s for %s from %I",
				rrname(repp->qd->type, tname, sizeof tname),
				repp->qd->owner->name, srcip);
			repp->flags = Runimplimented | Fresp | recursionflag |
				Oquery;
			return;
		}
	} else
		if(norecursion) {
			/* we don't recurse and we're not authoritative */
			repp->flags = Rok | Fresp | Oquery;
			return;
		}

	/*
	 *  get the answer if we can
	 */
	if(reqp->flags & Frecurse)
		neg = doextquery(repp, req, Recurse);
	else
		neg = doextquery(repp, req, Dontrecurse);

	/* authority is transitive */
	if(myarea != nil || (repp->an && repp->an->auth))
		repp->flags |= Fauth;

	/* pass on error codes */
	if(repp->an == nil){
		dp = dnlookup(repp->qd->owner->name, repp->qd->owner->class, 0);
		if(dp->rr == nil)
			if(reqp->flags & Frecurse)
				repp->flags |= dp->respcode | Fauth;
	}

	if(myarea == nil)
		/*
		 *  add name server if we know
		 */
		for(cp = repp->qd->owner->name; cp; cp = walkup(cp)){
			nsdp = dnlookup(cp, repp->qd->owner->class, 0);
			if(nsdp == nil)
				continue;

			repp->ns = rrlookup(nsdp, Tns, OKneg);
			if(repp->ns){
				/* don't pass on anything we know is wrong */
				if(repp->ns->negative){
					rrfreelist(repp->ns);
					repp->ns = nil;
				}
				break;
			}

			repp->ns = dblookup(cp, repp->qd->owner->class, Tns, 0, 0);
			if(repp->ns)
				break;
		}

	/*
	 *  add ip addresses as hints
	 */
	if(repp->qd->type != Taxfr && repp->qd->type != Tixfr){
		for(tp = repp->ns; tp; tp = tp->next)
			hint(&repp->ar, tp);
		for(tp = repp->an; tp; tp = tp->next)
			hint(&repp->ar, tp);
	}

	/*
	 *  add an soa to the authority section to help client
	 *  with negative caching
	 */
	if(repp->an == nil)
		if(myarea != nil){
			rrcopy(myarea->soarr, &tp);
			rrcat(&repp->ns, tp);
		} else if(neg != nil) {
			if(neg->negsoaowner != nil)
				rrcat(&repp->ns, rrlookup(neg->negsoaowner,
					Tsoa, NOneg));
			repp->flags |= neg->negrcode;
		}

	/*
	 *  get rid of duplicates
	 */
	unique(repp->an);
	unique(repp->ns);
	unique(repp->ar);

	rrfreelist(neg);

	dncheck(nil, 1);
}

/*
 *  satisfy a recursive request.  dnlookup will handle cnames.
 */
static RR*
doextquery(DNSmsg *mp, Request *req, int recurse)
{
	int type;
	char *name;
	RR *rp, *neg;

	name = mp->qd->owner->name;
	type = mp->qd->type;
	rp = dnresolve(name, Cin, type, req, &mp->an, 0, recurse, 1, 0);

	/* don't return soa hints as answers, it's wrong */
	if(rp && rp->db && !rp->auth && rp->type == Tsoa)
		rrfreelist(rp);

	/* don't let negative cached entries escape */
	neg = rrremneg(&rp);
	rrcat(&mp->an, rp);
	return neg;
}

static void
hint(RR **last, RR *rp)
{
	RR *hp;

	switch(rp->type){
	case Tns:
	case Tmx:
	case Tmb:
	case Tmf:
	case Tmd:
		hp = rrlookup(rp->host, Ta, NOneg);
		if(hp == nil)
			hp = dblookup(rp->host->name, Cin, Ta, 0, 0);
		rrcat(last, hp);
		break;
	}
}
