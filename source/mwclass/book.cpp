// Books and scrolls (OpenMW's MWClass::Book): reading one, and its tooltip
#include "mwbase/session.hpp"
#include "mwclass/classes.hpp"

void MWClass::Book::activate(Session& s, int ref)
{
	Ref& r = s.w.refs[ref];
	s.openScreen(SCR_BOOK);
	s.bookRef = ref;
	s.playSound(-1, r.obj && r.obj->scroll ? "scroll" : "Book Open");
}

void MWClass::Book::toolTipContents(Session& s, const Object& o)
{
	if (o.bookSkill >= 0)
		s.tip.emplace_back(o.scroll ? "Scroll" : "Book", col::textDim);
}
