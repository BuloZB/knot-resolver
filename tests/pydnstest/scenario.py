import dns.message
import dns.rrset
import dns.rcode
import dns.dnssec

class Entry:
    """
    Data entry represents scripted message and extra metadata, notably match criteria and reply adjustments.
    """

    # Globals
    default_ttl = 3600
    default_cls = 'IN'
    default_rc = 'NOERROR'

    def __init__(self):
        """ Initialize data entry. """
        self.match_fields = ['opcode', 'qtype', 'qname']
        self.adjust_fields = ['copy_id']
        self.origin = '.'
        self.message = dns.message.Message()
        self.sections = []

    def match_part(self, code, msg):
        """ Compare scripted reply to given message using single criteria. """
        if code not in self.match_fields and 'all' not in self.match_fields:
            return True
        expected = self.message
        if code == 'opcode':
            return self.__compare_val(expected.opcode(), msg.opcode())
        elif code == 'qtype':
            if len(expected.question) == 0:
                return True
            return self.__compare_val(expected.question[0].rdtype, msg.question[0].rdtype)
        elif code == 'qname':
            if len(expected.question) == 0:
                return True
            return self.__compare_val(expected.question[0].name, msg.question[0].name)
        elif code == 'subdomain':
            if len(expected.question) == 0:
                return True
            return self.__compare_sub(expected.question[0].name, msg.question[0].name)
        elif code == 'flags':
            return self.__compare_val(dns.flags.to_text(expected.flags), dns.flags.to_text(msg.flags))
        elif code == 'question':
            return self.__compare_rrs(expected.question, msg.question)
        elif code == 'answer':
            return self.__compare_rrs(expected.answer, msg.answer)
        elif code == 'authority':
            return self.__compare_rrs(expected.authority, msg.authority)
        elif code == 'additional':
            return self.__compare_rrs(expected.additional, msg.additional)
        else:
            raise Exception('unknown match request "%s"' % code)

    def match(self, msg):
        """ Compare scripted reply to given message based on match criteria. """
        match_fields = self.match_fields
        if 'all' in match_fields:
            match_fields = tuple(['flags'] + self.sections)
        for code in match_fields:
            try:
                self.match_part(code, msg)
            except Exception as e:
                raise Exception("%s: %s" % (code, str(e)))

    def set_match(self, fields):
        """ Set conditions for message comparison [all, flags, question, answer, authority, additional] """
        self.match_fields = fields

    def adjust_reply(self, query):
        """ Copy scripted reply and adjust to received query. """
        answer = dns.message.from_text(self.message.to_text())
        if 'copy_id' in self.adjust_fields:
            answer.id = query.id
        if 'copy_query' in self.adjust_fields:
            answer.question = query.question
        return answer

    def set_adjust(self, fields):
        """ Set reply adjustment fields [copy_id, copy_query] """
        self.adjust_fields = fields

    def set_reply(self, fields):
        """ Set reply flags and rcode. """
        eflags = []
        flags = []
        rcode = dns.rcode.from_text(self.default_rc)
        for code in fields:
            if code == 'DO':
                eflags.append(code)
                continue
            try:
                rcode = dns.rcode.from_text(code)
            except:
                flags.append(code)
        self.message.flags = dns.flags.from_text(' '.join(flags))
        self.message.ednsflags = dns.flags.edns_from_text(' '.join(eflags))
        self.message.set_rcode(rcode)

    def begin_section(self, section):
        """ Begin packet section. """
        self.section = section
        self.sections.append(section.lower())

    def add_record(self, owner, args):
        """ Add record to current packet section. """
        rr = self.__rr_from_str(owner, args)
        if self.section == 'QUESTION':
            self.message.question.append(rr)
        elif self.section == 'ANSWER':
            self.message.answer.append(rr)
        elif self.section == 'AUTHORITY':
            self.message.authority.append(rr)
        elif self.section == 'ADDITIONAL':
            self.message.additional.append(rr)
        else:
            raise Exception('bad section %s' % self.section)

    def __rr_from_str(self, owner, args):
        """ Parse RR from tokenized string. """
        if not owner.endswith('.'):
            owner += self.origin
        ttl = self.default_ttl
        rdclass = self.default_cls
        try:
            ttl = dns.ttl.from_text(args[0])
            args.pop(0)
        except:
            pass  # optional
        try:
            rdclass = dns.rdataclass.from_text(args[0])
            args.pop(0)
        except:
            pass  # optional
        rdtype = args.pop(0)
        rr = dns.rrset.from_text(owner, ttl, rdclass, rdtype)
        if len(args) > 0:
            if (rr.rdtype == dns.rdatatype.DS):
                # convert textual algorithm identifier to number
                args[1] = str(dns.dnssec.algorithm_from_text(args[1]))
            rd = dns.rdata.from_text(rr.rdclass, rr.rdtype, ' '.join(args), origin=dns.name.from_text(self.origin), relativize=False)
            rr.add(rd)
        return rr

    def __compare_rrs(self, expected, got):
        """ Compare lists of RR sets, throw exception if different. """
        for rr in expected:
            if rr not in got:
                raise Exception("expected record '%s'" % rr.to_text())
        for rr in got:
            if rr not in expected:
                raise Exception("unexpected record '%s'" % rr.to_text())
        return True

    def __compare_val(self, expected, got):
        """ Compare values, throw exception if different. """
        if expected != got:
            raise Exception("expected '%s', got '%s'" % (expected, got))
        return True

    def __compare_sub(self, got, expected):
        """ Check if got subdomain of expected, throw exception if different. """
        if not expected.is_subdomain(got):
            raise Exception("expected subdomain of '%s', got '%s'" % (expected, got))
        return True


class Range:
    """
    Range represents a set of scripted queries valid for given step range.
    """

    def __init__(self, a, b):
        """ Initialize reply range. """
        self.a = a
        self.b = b
        self.address = None
        self.stored = []

    def add(self, entry):
        """ Append a scripted response to the range"""
        self.stored.append(entry)

    def eligible(self, id, address):
        """ Return true if this range is eligible for fetching reply. """
        if self.a <= id <= self.b:
            return None in (self.address, address) or (self.address == address)
        return False

    def reply(self, query):
        """ Find matching response to given query. """
        for candidate in self.stored:
            try:
                candidate.match(query)
                return candidate.adjust_reply(query)
            except Exception as e:
                pass
        return None


class Step:
    """
    Step represents one scripted action in a given moment,
    each step has an order identifier, type and optionally data entry.
    """

    require_data = ['TIME_PASSES']

    def __init__(self, id, type, extra_args):
        """ Initialize single scenario step. """
        self.id = int(id)
        self.type = type
        self.args = extra_args
        self.data = []
        self.has_data = self.type not in Step.require_data

    def add(self, entry):
        """ Append a data entry to this step. """
        self.data.append(entry)

    def play(self, ctx):
        """ Play one step from a scenario. """
        if self.type == 'QUERY':
            return self.__query(ctx)
        elif self.type == 'CHECK_OUT_QUERY':
             pass # Ignore
        elif self.type == 'CHECK_ANSWER':
            return self.__check_answer(ctx)
        elif self.type == 'TIME_PASSES':
            return self.__time_passes(ctx)
        elif self.type == 'REPLY':
            pass
        else:
            raise Exception('step %s unsupported' % self.type)

    def __check_answer(self, ctx):
        """ Compare answer from previously resolved query. """
        if len(self.data) == 0:
            raise Exception("response definition required")
        if ctx.last_answer is None:
            raise Exception("no answer from preceding query")
        expected = self.data[0]
        expected.match(ctx.last_answer)

    def __query(self, ctx):
        """ Resolve a query. """
        if len(self.data) == 0:
            raise Exception("query definition required")
        msg = self.data[0].message
        msg.use_edns(edns = 1)
        self.answer = ctx.resolve(msg.to_wire())
        if self.answer is not None:
            self.answer = dns.message.from_wire(self.answer)
            ctx.last_answer = self.answer

    def __time_passes(self, ctx):
        """ Modify system time. """
        ctx.scenario.time = int(self.args[1])
        ctx.set_time(ctx.scenario.time)

class Scenario:
    def __init__(self, info):
        """ Initialize scenario with description. """
        self.info = info
        self.ranges = []
        self.steps = []
        self.current_step = None

    def reply(self, query, address = None):
        """ Attempt to find a range reply for a query. """
        step_id = 0
        if self.current_step is not None:
            step_id = self.current_step.id
        # Unknown address, select any match
        # TODO: workaround until the server supports stub zones
        if address not in [rng.address for rng in self.ranges]:
            address = None
        # Find current valid query response range
        for rng in self.ranges:
            if rng.eligible(step_id, address):
                return rng.reply(query)
        # Find any prescripted one-shot replies
        for step in self.steps:
            if step.id <= step_id or step.type != 'REPLY':
                continue
            try:
                candidate = step.data[0]
                candidate.match(query)
                step.data.remove(candidate)
                return candidate.adjust_reply(query)
            except:
                pass

    def play(self, ctx):
        """ Play given scenario. """
        step = None
        if len(self.steps) == 0:
            raise ('no steps in this scenario')
        try:
            ctx.scenario = self
            for step in self.steps:
                self.current_step = step
                step.play(ctx)
        except Exception as e:
            raise Exception('step #%d %s' % (step.id, str(e)))
