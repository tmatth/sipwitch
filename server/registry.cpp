// Copyright (C) 2006-2007 David Sugar, Tycho Softworks.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "server.h"

NAMESPACE_SIPWITCH
using namespace UCOMMON_NAMESPACE;

static volatile unsigned active_routes = 0;
static volatile unsigned active_entries = 0;
static volatile unsigned active_targets = 0;
static volatile unsigned published_routes = 0;
static volatile unsigned allocated_routes = 0;
static volatile unsigned allocated_targets = 0;
static unsigned mapped_entries = 999;

static unsigned priorities = 10;
static unsigned keysize = 177;
static MappedRegistry **extmap = NULL;
static LinkedObject **addresses = NULL;
static LinkedObject **published = NULL;
static LinkedObject **contacts = NULL;
static LinkedObject **primap = NULL;
static LinkedObject *freeroutes = NULL;
static LinkedObject *freetargets = NULL;
static LinkedObject **keys = NULL;
static mutex_t targetlock;
static mutex_t *reglock;

registry registry::reg;

registry::registry() :
service::callback(0), mapped_reuse<MappedRegistry>()
{
	realm = "Local Telephony";
	digest = "md5";
	prefix = 100;
	range = 600;
	expires = 300l;
}

void registry::exclusive(MappedRegistry *rr)
{
	unsigned idx;
	if(!rr || !reglock) {
		process::errlog(DEBUG1, "invalid lock reference");
		return;
	}
	idx = getIndex(rr);
	if(idx >= mapped_entries) {
		process::errlog(ERROR, "lock out of range");
		return;
	}
	reglock[idx].acquire();
}

MappedRegistry *registry::find(const char *id)
{
	linked_pointer<MappedRegistry> rp;
	unsigned path = NamedObject::keyindex(id, keysize);
	if(!keys)
		return NULL;
	rp = keys[path];
	while(rp) {
		if(!strcmp(rp->userid, id))
			break;
		rp.next();
	}
	return *rp;
}

unsigned registry::getIndex(MappedRegistry *rr)
{
	unsigned x = (unsigned)(((caddr_t)rr - reg.getStart()) / sizeof(MappedRegistry));
	return x;
}

void registry::start(service *cfg)
{
	process::errlog(DEBUG1, "registry starting; mapping %d entries", mapped_entries);
	MappedReuse::create("sipwitch.regmap", mapped_entries);
	if(!reg)
		process::errlog(FAILURE, "registry could not be mapped");
	initialize();
	reglock = new mutex_t[mapped_entries];
}

bool registry::check(void)
{
	process::errlog(INFO, "checking registry...");
	reg.exlock();
	reg.unlock();
	return true;
}

void registry::stop(service *cfg)
{
	process::errlog(DEBUG1, "registry stopping");
	MappedMemory::release();
	MappedMemory::remove("sipwitch.regmap");
}

void registry::snapshot(FILE *fp) 
{
	MappedRegistry *rr;
	unsigned count = 0;
	time_t now;
	linked_pointer<target> tp;
	linked_pointer<route> rp;
	char buffer[128];

	access();
	fprintf(fp, "Registry:\n"); 
	fprintf(fp, "  mapped entries: %d\n", mapped_entries);
	fprintf(fp, "  active entries: %d\n", active_entries);
	fprintf(fp, "  active routes:  %d\n", active_routes);
	fprintf(fp, "  active targets: %d\n", active_targets);
	fprintf(fp, "  published routes:  %d\n", published_routes);
	fprintf(fp, "  allocated routes:  %d\n", allocated_routes);
	fprintf(fp, "  allocated targets: %d\n", allocated_targets);

	while(count < mapped_entries) {
		time(&now);
		rr = reg.pos(count++);
		exclusive(rr);
		if(rr->type != REG_EXPIRED && (!rr->expires || rr->expires >= now)) {
			if(rr->ext)
				snprintf(buffer, sizeof(buffer), "%d", rr->ext);
			else
				string::set(buffer, sizeof(buffer), "none");
			if(rr->type == REG_USER)
				fprintf(fp, "  user %s; extension=%s, profile=%s,",
					rr->userid, buffer, rr->profile.id);
			else if(rr->type == REG_GATEWAY)
				fprintf(fp, "  gateway %s;", rr->userid);
			else if(rr->type == REG_SERVICE)
				fprintf(fp, "  service %s;", rr->userid);
			else if(rr->type == REG_REFER)
				fprintf(fp, "  refer %s; extensions=%s,",
					rr->userid, buffer);
			else if(rr->type == REG_REJECT)
				fprintf(fp, "  reject %s; extension=%s,",
					rr->userid, buffer);
			if(!rr->count)
				fprintf(fp, " address=none\n");
			else
				fputc('\n', fp);
			tp = rr->targets;
			while(tp) {
				Socket::getaddress((struct sockaddr *)(&tp->address), buffer, sizeof(buffer));
				fprintf(fp, "    address=%s, contact=%s", buffer, tp->contact);		
				if(tp->expires && tp->expires <= now)
					fprintf(fp, ", expired");
				else if(tp->expires)
					fprintf(fp, ", expires %ld second(s)", tp->expires - now);
				if(tp.getNext())
					fputc(',', fp);
				fputc('\n', fp);
				tp.next();
			}
			rp = rr->routes;
			if(rp && rr->type == REG_SERVICE)
				fprintf(fp, "      services=");
			else if(rp && rr->type == REG_GATEWAY)
				fprintf(fp, "      routes=");
			while(rp && (rr->type == REG_SERVICE || rr->type == REG_GATEWAY)) {
				fputs(rp->entry.text, fp);
				if(rp->getNext())
					fputc(',', fp);
				else
					fputc('\n', fp);
				rp.next();
			}
			rp = rr->published;
			if(rp)
				fprintf(fp, "      published=");
			while(rp) {
				fputs(rp->entry.text, fp);
				if(rp->getNext())
					fputc(',', fp);
				else
					fputc('\n', fp);
				rp.next();
			}
		}
		reglock[getIndex(rr)].release();
		fflush(fp);
		Thread::yield();
	}
	release();
} 

bool registry::remove(const char *id)
{
	bool rtn = true;
	MappedRegistry *rr;

	reg.exlock();
	rr = find(id);
	if(rr)
		expire(rr);
	else
		rtn = false;
	reg.unlock();
	return rtn;
}

void registry::expire(MappedRegistry *rr)
{
	linked_pointer<target> tp = rr->targets;
	linked_pointer<route> rp = rr->routes;
	unsigned path;

	--active_entries;

	while(rp) {
		route *nr = rp.getNext();
		--active_routes;
		if(rr->type == REG_SERVICE) {
			path = NamedObject::keyindex(rp->entry.text, keysize);
			rp->entry.delist(&contacts[path]);
		}
		else
			rp->entry.delist(&primap[rp->entry.priority]);
		rp->entry.text[0] = 0;
		rp->enlist(&freeroutes);
		rp = nr;
	}	
	rp = rr->published;
	while(rp) {
		route *nr = rp.getNext();
		--active_routes;
		--published_routes;
		path = NamedObject::keyindex(rp->entry.text, keysize);
		rp->entry.delist(&published[path]);
		rp->entry.text[0] = 0;
		rp->enlist(&freeroutes);
		rp = nr;
	}		
	while(tp) {
		// if active address index, delist & clear it
		if(tp->index.address) {
			path = Socket::keyindex(tp->index.address, keysize);
			tp->index.delist(&addresses[path]);
			tp->index.address = NULL;
			tp->index.registry = NULL;
		}
		target *nt = tp.getNext();
		--active_targets;
		targetlock.acquire();
		tp->enlist(&freetargets);
		targetlock.release();
		tp = nt;
	}
	rr->routes = NULL;
	rr->targets = NULL;
	rr->published = NULL;
	rr->count = 0;
	if(reg.range && rr->ext) {
		if(extmap[rr->ext - reg.prefix] == rr) {
			process::errlog(INFO, "expiring %s from extension %u", rr->userid, rr->ext);
			service::publish(NULL, "- release %u %s %u", rr->ext, rr->userid, getIndex(rr));
			extmap[rr->ext - reg.prefix] = NULL;
		}
		else
			goto hold;
	}
	else
		process::errlog(INFO, "expiring %s", rr->userid);

	path = NamedObject::keyindex(rr->userid, keysize);
	rr->ext = 0;
	rr->userid[0] = 0;
	rr->type = REG_EXPIRED;
	rp->delist(&keys[path]);

hold:
	reg.removeLocked(rr);
}

void registry::cleanup(void)
{
	MappedRegistry *rr;
	unsigned count = 0;
	time_t now;

	while(count < mapped_entries) {
		time(&now);
		rr = reg.pos(count++);
		reg.exlock();
		if(rr->type != REG_EXPIRED && rr->expires && rr->expires < now)
			expire(rr);
		reg.unlock();
		Thread::yield();
	}
}

bool registry::reload(service *cfg)
{
	const char *key = NULL, *value;
	linked_pointer<service::keynode> sp = cfg->getList("registry");

	while(sp) {
		key = sp->getId();
		value = sp->getPointer();
		if(key && value) {
			if(!stricmp(key, "mapped") && !isConfigured()) 
				mapped_entries = atoi(value);
			else if(!stricmp(key, "digest") && !isConfigured()) {
				digest = strdup(value);
				string::upper((char *)digest);
			}
			else if(!stricmp(key, "realm") && !isConfigured())
				realm = strdup(value);
			else if(!stricmp(key, "prefix") && !isConfigured())
				prefix = atoi(value);
			else if(!stricmp(key, "range") && !isConfigured())
				range = atoi(value);
			else if(!stricmp(key, "priorities") && !isConfigured())
				priorities = atoi(value);
			else if(!stricmp(key, "expires"))
				expires = atoi(value);
			else if(!stricmp(key, "keysize") && !isConfigured())
				keysize = atoi(value);
		}
		sp.next();
	}

	if(isConfigured())
		return true;

	if(range) {
		extmap = new MappedRegistry *[range];
		memset(extmap, 0, sizeof(MappedRegistry *) * range);
	}
	primap = new LinkedObject *[priorities];
	memset(primap, 0, sizeof(LinkedObject *) * priorities);
	keys = new LinkedObject *[keysize];
	contacts = new LinkedObject *[keysize];
	published = new LinkedObject *[keysize];
	addresses = new LinkedObject *[keysize];
	memset(keys, 0, sizeof(LinkedObject *) * keysize);
	memset(contacts, 0, sizeof(LinkedObject *) * keysize);
	memset(published, 0, sizeof(LinkedObject *) * keysize);
	memset(addresses, 0, sizeof(LinkedObject *) * keysize);
	process::errlog(INFO, "realm %s", realm);
	return true;
}

unsigned registry::getEntries(void)
{
	return mapped_entries;
}

MappedRegistry *registry::modify(const char *id)
{
	MappedRegistry *rr;
	unsigned ext = atoi(id);

	reg.exlock();
	rr = find(id);
	if(!rr && reg.range && ext >= reg.prefix && ext < reg.prefix + reg.range)
		rr = extmap[ext - reg.prefix];

	if(!rr)
		reg.unlock();
	return rr;
}

MappedRegistry *registry::create(const char *id)
{
	MappedRegistry *rr, *prior;
	unsigned path = NamedObject::keyindex(id, keysize);
	linked_pointer<service::keynode> rp;
	service::keynode *node, *leaf;
	unsigned ext = 0;
	const char *cp = "none";
	profile_t *pro = NULL;

	reg.exlock();
	rr = find(id);
	if(rr) {
		exclusive(rr);
		reg.share();
		return rr;
	}

	rr = reg.getLocked();
	if(!rr) {
		reg.unlock();
		return NULL;
	}

	node = config::getProvision(id);
	cp = "none";
	rr->type = REG_EXPIRED;
	rr->expires = 0;

	if(node)
		cp = node->getId();
	if(!stricmp(cp, "user"))
		rr->type = REG_USER;
	else if(!stricmp(cp, "refer"))
		rr->type = REG_REFER;
	else if(!stricmp(cp, "reject"))
		rr->type = REG_REJECT;
	else if(!stricmp(cp, "gateway"))
		rr->type = REG_GATEWAY;
	else if(!stricmp(cp, "service"))
		rr->type = REG_SERVICE;
	if(!node || rr->type == REG_EXPIRED) {
		config::release(node);
		reg.removeLocked(rr);
		reg.unlock();
		return NULL;
	}

	// add static services if exist
	rp = node->leaf("contacts");
	if(rp)
		rp = rp->getFirst();

	while(rp) {
		if(!stricmp(rp->getId(), "contact") && rp->getPointer())
			addContact(rr, rp->getPointer());
		rp.next();
	}

	// add published uris
	rp = node->leaf("published");
	if(!rp)
		node->leaf("publish");

	if(rp && rp->getPointer())
		addPublished(rr, rp->getPointer());

	if(rp && !rp->getPointer() && !rp->getFirst())
		addPublished(rr, rr->userid);

	if(rp)
		rp = rp->getFirst();

	while(rp) {
		if(!stricmp(rp->getId(), "contact") && rp->getPointer())
			addPublished(rr, rp->getPointer());
		rp.next();
	}
	
	// we add routes while still exclusive owner of registry since
	// they update priority indexes.
	rp = node->leaf("routes");
	if(rp)
		rp = rp->getFirst();
	
	while(rp) {
		const char *pattern = NULL, *prefix = NULL, *suffix = NULL;
		unsigned priority = 0;
		linked_pointer<service::keynode> route;

		route = (LinkedObject*)NULL;
		if(!stricmp(rp->getId(), "route"))
			route = rp->getFirst();
		while(route) {
			const char *id = route->getId();
			const char *value = route->getPointer();

			if(id && value && !stricmp(id, "pattern"))
				pattern = value;
			else if(id && value && !stricmp(id, "priority"))
				priority = atoi(value);
			else if(id && value && !stricmp(id, "prefix"))
				prefix = value;
			else if(id && value && !stricmp(id, "suffix"))
				suffix = value;
			route.next();
		}
		if(pattern)
			addRoute(rr, pattern, priority, prefix, suffix);
		rp.next();
	}

	leaf = node->leaf("extension");
	if(leaf && leaf->getPointer())
		ext = atoi(leaf->getPointer());

	if(rr->type == REG_USER) {
		pro = NULL;
		leaf = node->leaf("profile");
		if(leaf)
			pro = config::getProfile(leaf->getPointer());
		if(!pro)
			pro = config::getProfile("*");
		if(pro)
			memcpy(&rr->profile, pro, sizeof(rr->profile));
	}

	config::release(node);
	strcpy(rr->userid, id);
	rr->ext = 0;
	rr->enlist(&keys[path]);
	if(ext >= reg.prefix && ext < reg.prefix + reg.range) {
		prior = extmap[ext - reg.prefix];
		if(prior && prior != rr) {
			process::errlog(INFO, "releasing %s from extension %d", prior->userid, ext);
			service::publish(NULL, "- release %u %s %u", ext, prior->userid, getIndex(rr)); 
			prior->ext = 0;
		}
		extmap[ext - reg.prefix] = rr;
		rr->ext = ext;
		process::errlog(INFO, "activating %s as extension %d", rr->userid, ext);
		service::publish(NULL, "- activate %u %s %u", ext, rr->userid, getIndex(rr));
	}
	++active_entries;

	// exchange exclusive mutex lock for registry to shared before return
	// when registry state is again stable.

	exclusive(rr);
	reg.share();

	return rr;
}	

MappedRegistry *registry::address(struct sockaddr *addr)
{
	target *target;
	linked_pointer<target::indexing> ind;
	MappedRegistry *rr = NULL;
	unsigned path = Socket::keyindex(addr, keysize);
	time_t now;

	reg.access();

	time(&now);
	ind = addresses[path];

	while(ind) {
		target = ind->getTarget();
		if(target && target->expires > now && Socket::equal(addr, ind->address)) {
			rr = ind->registry;
			break;
		}
		ind.next();
	}

	if(!rr)
		reg.release();

	return rr;
}


MappedRegistry *registry::contact(const char *uri)
{
	MappedRegistry *rr = NULL;
	struct sockaddr *addr = NULL;
	stack::address *target = NULL;
	char buffer[MAX_USERID_SIZE];
	char *cp;

	if(!strnicmp(uri, "sip:", 4))
		uri += 4;
	else if(!strnicmp(uri, "sips:", 5))
		uri += 5;

	string::set(buffer, sizeof(buffer), uri);
	cp = strchr(buffer, '@');
	if(cp)
		*cp = 0;
	if(strchr(uri, '@')) {
		target = stack::getAddress(uri);
		if(target)
			addr = target->getAddr();
	}

	if(addr)
		rr = contact(addr, buffer);

	if(target)
		delete target;

	return rr;
}

MappedRegistry *registry::contact(struct sockaddr *addr, const char *uid)
{
	linked_pointer<route> rp;
	unsigned path = NamedObject::keyindex(uid, keysize);
	reg.access();
	rp = contacts[path];
	while(rp) {
		if(!stricmp(uid, rp->entry.text) && Socket::equal(addr, (struct sockaddr *)&rp->entry.registry->contact))
			break;
		rp.next();
	}

	if(!rp) {
		reg.release();
		return NULL;
	}
	return rp->entry.registry;
}

bool registry::isExtension(const char *id)
{
	unsigned ext = atoi(id);
	
	while(*id) {
		if(*id < '0' || *id > '9')
			return false;
		++id;
	}

	if(!reg.range)
		return false;

	if(ext >= reg.prefix && ext < reg.prefix + reg.range)
		return true;

	return false;
}
	
MappedRegistry *registry::getExtension(const char *id)
{
	unsigned ext = atoi(id);
	MappedRegistry *rr = NULL;
	time_t now;

	reg.access();
	time(&now);
	rr = extmap[ext - reg.prefix];
	if(rr->expires && rr->expires < now)
		rr = NULL;
	if(!rr)
		reg.MappedReuse::release();
	else
		exclusive(rr);
	return rr;
}

MappedRegistry *registry::access(const char *id)
{
	MappedRegistry *rr;
	unsigned ext = 0;

	if(isExtension(id))
		ext = atoi(id);

	reg.access();
	rr = find(id);
	if(!rr && reg.range && ext >= reg.prefix && ext < reg.prefix + reg.range)
		rr = extmap[ext - reg.prefix];
	if(!rr)
		reg.release();
	else
		exclusive(rr);
	return rr;
}

void registry::update(MappedRegistry *rr)
{
	if(!rr)
		return;

	reg.unlock();
}

void registry::release(MappedRegistry *rr)
{
	if(!rr)
		return;

	reglock[getIndex(rr)].release();
	reg.release();
}

unsigned registry::setTarget(MappedRegistry *rr, stack::address *addr, time_t expires, const char *contact)
{
	stack::address *origin = NULL;
	struct sockaddr *ai, *oi = NULL;
	linked_pointer<target> tp = rr->targets;
	socklen_t len;
	bool created = false;

	if(!addr)
		return 0;
	ai = addr->getAddr();
	if(!ai)
		return 0;

	len = Socket::getlen(ai);

	while(tp && rr->count > 1) {
		--active_targets;
		targetlock.acquire();
		tp->enlist(&freetargets);
		targetlock.release();
		tp.next();
		--rr->count;
	}

	if(!tp) {
		tp = createTarget();
		tp->enlist(&rr->targets);
		rr->count = 1;
		tp->address.sa_family = 0;
		created = true;
	}
	rr->expires = tp->expires = expires;
	if(!Socket::equal((struct sockaddr *)(&tp->address), ai)) {
		if(tp->index.address) {
			tp->index.delist(&addresses[Socket::keyindex(tp->index.address, keysize)]);
			tp->index.address = NULL;
			tp->index.registry = NULL;
			created = true;
		}
		
		origin = stack::getAddress(contact);
		if(origin)
			oi = origin->getAddr();
		if(!oi)
			oi = ai;
		memcpy(&tp->address, ai, len);
		memcpy(&rr->contact, oi, len);
		if(created) {
			tp->index.registry = rr;
			tp->index.address = (struct sockaddr *)&tp->address;
			tp->index.enlist(&addresses[Socket::keyindex(tp->index.address, keysize)]);
		}
		Socket::getinterface((struct sockaddr *)&tp->interface, ((struct sockaddr *)&tp->address));
		if(origin)
			delete origin;
	}
	string::set(tp->contact, MAX_URI_SIZE, contact);
	return 1;
}

void registry::addRoute(MappedRegistry *rr, const char *pat, unsigned pri, const char *prefix, const char *suffix)
{
	char buffer[MAX_USERID_SIZE];
	route *rp = createRoute();

	if(!prefix)
		prefix = "";
	if(!suffix)
		suffix = "";

	snprintf(buffer, sizeof(buffer), "%s;%s,%s", pat, prefix, suffix);
	string::set(rp->entry.text, MAX_USERID_SIZE, buffer);
	rp->entry.priority = pri;
	rp->entry.registry = rr;
	rp->entry.enlist(&primap[pri]);
	rp->enlist(&rr->routes);
}

void registry::addPublished(MappedRegistry *rr, const char *id)
{
	unsigned path = NamedObject::keyindex(id, keysize);
	route *rp = createRoute();
	string::set(rp->entry.text, MAX_USERID_SIZE, id);
	rp->entry.priority = 0;
	rp->entry.registry = rr;
	rp->entry.enlist(&published[path]);
	rp->enlist(&rr->published);
	++published_routes;
}

void registry::addContact(MappedRegistry *rr, const char *id)
{
	unsigned path = NamedObject::keyindex(id, keysize);

	route *rp = createRoute();
	string::set(rp->entry.text, MAX_USERID_SIZE, id);
	rp->entry.priority = 0;
	rp->entry.registry = rr;
	rp->entry.enlist(&contacts[path]);
	rp->enlist(&rr->routes);
}

unsigned registry::addTarget(MappedRegistry *rr, stack::address *addr, time_t expires, const char *contact)
{
	stack::address *origin;
	struct sockaddr *ai, *oi = NULL;
	linked_pointer<target> tp = rr->targets;
	target *expired = NULL;
	time_t now;
	socklen_t len;

	if(!addr)
		return 0;
	ai = addr->getAddr();
	if(!ai)
		return 0;

	if(expires > rr->expires)
		rr->expires = expires;

	len = Socket::getlen(ai);
	time(&now);
	while(tp) {
		if(tp->expires < now)
			expired = *tp;
		if(Socket::equal((struct sockaddr *)(&tp->address), ai))
			break;
		tp.next();
	} 
	if(tp) {
		string::set(tp->contact, MAX_URI_SIZE, contact);
		if(expired && expired != *tp) {
			if(expired->index.address) {
				expired->index.delist(&addresses[Socket::keyindex(expired->index.address, keysize)]);
				expired->index.address = NULL;
				expired->index.registry = NULL;
			}
			expired->delist(&rr->targets);
			--rr->count;
			--active_targets;
			targetlock.acquire();
			expired->enlist(&freetargets);
			targetlock.release();
		}
		tp->expires = expires;
		return rr->count;
	}
	if(!expired) {
		origin = stack::getAddress(contact);
		if(origin)
			oi = origin->getAddr();
		if(!oi)
			oi = ai;
		expired = createTarget();
		expired->enlist(&rr->targets);
		memcpy(&rr->contact, oi, len);
		if(origin)
			delete origin;
		++rr->count;
	}
	string::set(expired->contact, sizeof(expired->contact), contact);
	expired->expires = expires;
	memcpy(&expired->address, ai, len);
	Socket::getinterface((struct sockaddr *)&expired->interface, (struct sockaddr *)&expired->address);
	expired->index.registry = rr;
	expired->index.address = (struct sockaddr *)&expired->address;
	expired->index.enlist(&addresses[Socket::keyindex(expired->index.address, keysize)]); 
	return rr->count;
}

unsigned registry::setTargets(MappedRegistry *rr, stack::address *addr)
{
	struct addrinfo *al;
	linked_pointer<target> tp = rr->targets;
	socklen_t len;

	if(!addr)
		return 0;

	al = addr->getList();
	if(!al)
		return 0;

	if(rr->expires)
		return 0;

	while(tp) {
		--active_targets;
		targetlock.acquire();
		tp->enlist(&freetargets);
		targetlock.release();
		tp.next();
	}	
	rr->targets = NULL;
	rr->count = 0;
	while(al) {
		len = Socket::getlen(al->ai_addr);

		tp = createTarget();
		memcpy(&tp->address, al->ai_addr, len);
		memcpy(&rr->contact, &tp->address, len);
		Socket::getinterface((struct sockaddr *)&tp->interface, (struct sockaddr *)&tp->address);
		stack::sipAddress(&tp->address, tp->contact, rr->userid);
		tp->expires = 0l;
		tp->enlist(&rr->targets);
		++rr->count;
		al = al->ai_next;
	}
	rr->expires = 0;
	return rr->count;
}

registry::route *registry::createRoute(void)
{
	route *r;
	r = static_cast<route *>(freeroutes);
	if(r)
		freeroutes = r->getNext();
	if(!r) {
		++allocated_routes;
		r = static_cast<route *>(config::allocate(sizeof(route)));
	}
	++active_routes;
	return r;
}

registry::target *registry::target::indexing::getTarget(void)
{
	caddr_t cp = (caddr_t)address;
	target *tp = (target *)0;
	size_t offset = (size_t)(&tp->address);

	if(!address)
		return NULL;


	cp -= offset;
	return reinterpret_cast<target *>(cp);
}
	
registry::target *registry::createTarget(void)
{
	target *t;
	targetlock.acquire();
	t = static_cast<target *>(freetargets);
	if(t)
		freetargets = t->getNext();
	targetlock.release();
	if(!t) {
		++allocated_targets;
		t = static_cast<target *>(config::allocate(sizeof(target)));
	}
	++active_targets;
	return t;
}

END_NAMESPACE
