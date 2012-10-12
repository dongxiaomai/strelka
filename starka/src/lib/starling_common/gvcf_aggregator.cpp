// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Copyright (c) 2009-2012 Illumina, Inc.
//
// This software is covered by the "Illumina Genome Analyzer Software
// License Agreement" and the "Illumina Source Code License Agreement",
// and certain third party copyright/licenses, and any user of this
// source file is bound by the terms therein (see accompanying files
// Illumina_Genome_Analyzer_Software_License_Agreement.pdf and
// Illumina_Source_Code_License_Agreement.pdf and third party
// copyright/license notices).
//
//

/// \file
///
/// \author Chris Saunders
///

#include "gvcf_aggregator.hh"

#include <iostream>



static
void
set_site_gt(const diploid_genotype::result_set& rs,
            site_modifiers& smod) {

    smod.max_gt=rs.max_gt;
    smod.gqx=rs.max_gt_qphred;
}


static
void
set_site_filters(const starling_options& opt,
                 site_info& si) {

   if(opt.is_gvcf_min_gqx) {
        if(si.smod.gqx<opt.gvcf_min_gqx) si.smod.set_filter(VCF_FILTERS::LowGQX);
    }

    if(opt.is_gvcf_max_depth) {
        if((si.n_used_calls+si.n_unused_calls) > opt.gvcf_max_depth) si.smod.set_filter(VCF_FILTERS::HighDepth);
    }
}



static
void
add_site_modifiers(const starling_options& opt,
                   site_info& si) {

    si.smod.clear();

    si.smod.is_unknown=(si.ref=='N');

    si.smod.is_used_covered=(si.n_used_calls!=0);
    si.smod.is_covered=(si.smod.is_used_covered || si.n_unused_calls!=0);

    if     (si.smod.is_unknown) {
        si.smod.gqx=0;
        si.smod.max_gt=0;
    } else if(si.dgt.genome.max_gt != si.dgt.poly.max_gt) {
        // if these two disagree then GQX should be 0 and GT should be genome:
        si.smod.gqx=0;
        si.smod.max_gt=si.dgt.genome.max_gt;
    } else {
        if(si.dgt.genome.max_gt_qphred<si.dgt.poly.max_gt_qphred){
            set_site_gt(si.dgt.genome,si.smod);
        } else {
            set_site_gt(si.dgt.poly,si.smod);
        }
    }

    set_site_filters(opt,si);
}



gvcf_aggregator::
gvcf_aggregator(const starling_options& opt,
                const pos_range& report_range,
                const reference_contig_segment& ref,
                std::ostream* osptr)
    : _opt(opt)
    , _report_range(report_range.begin_pos,report_range.end_pos)
    , _ref(ref)
    , _osptr(osptr)
    , _chrom(NULL)
    , _indel_end_pos(0)
    , _indel_buffer_size(0)
    , _site_buffer_size(0)
    , _block(_opt)
    , _head_pos(report_range.begin_pos)
{
    assert(report_range.is_begin_pos);
    assert(report_range.is_end_pos);

    if(opt.is_gvcf_output()) {
        assert(NULL != osptr);
    }
    add_site_modifiers(_opt,_empty_site);
}



gvcf_aggregator::
~gvcf_aggregator() {
    skip_to_pos(_report_range.end_pos+1);
    process_overlaps();
}





// fill in missing sites:
void
gvcf_aggregator::
skip_to_pos(const pos_t target_pos) {

    // advance through any indel region by adding individual sites
    while(_head_pos<target_pos) {
        add_site_internal(get_empty_site(_head_pos));

        // only add one empty site after completing any pre-existing indel blocks,
        // then extend the block size of that one site as required:
        if(0 != _indel_buffer_size) continue;
        assert(_block.count!=0);
        _block.count += (target_pos-_head_pos);
        _head_pos=target_pos;
    }
}



void
gvcf_aggregator::
add_site(site_info& si) {

    skip_to_pos(si.pos);
    add_site_modifiers(_opt,si);

    add_site_internal(si);
}



void
gvcf_aggregator::
add_site_internal(const site_info& si) {

    _head_pos=si.pos+1;

    if(0 != _indel_buffer_size) {
        if(si.pos>=_indel_end_pos) {
            process_overlaps();
        } else {
            while(_site_buffer.size() <= _site_buffer_size) {
                _site_buffer.push_back(site_info());
            }
            _site_buffer[_site_buffer_size++] = si;
            return;
        }
    }

    // write_site:
    queue_site_record(si);
}



static
bool
is_het_indel(const starling_diploid_indel_core& dindel) {
    return (dindel.max_gt==STAR_DIINDEL::HET);
}

static
bool
is_no_indel(const starling_diploid_indel_core& dindel) {
    return (dindel.max_gt==STAR_DIINDEL::NOINDEL);
}



void
gvcf_aggregator::
add_indel(const pos_t pos,
          const indel_key ik,
          const starling_diploid_indel_core& dindel,
          const starling_indel_report_info& iri,
          const starling_indel_sample_report_info& isri) {

    // we can't handle breakends at all right now:
    if(ik.is_breakpoint()) return;

    // don't handle max_gt=="ref" cases for now:
    if(is_no_indel(dindel)) return;

    skip_to_pos(pos);

    // check to see if we add this indel to the buffer:
    if(0 != _indel_buffer_size) {
        // check if this indel overlaps the buffer -- note this deleberatly picks up adjacent deletions:
        if(pos<=_indel_end_pos) {
            _indel_end_pos=std::max(_indel_end_pos,ik.right_pos());
        } else {
            process_overlaps();
        }
    }

    while(_indel_buffer.size() <= _indel_buffer_size) {
        _indel_buffer.push_back(indel_info());
    }
    _indel_buffer[_indel_buffer_size++].init(pos,ik,dindel,iri,isri);
    _indel_end_pos=ik.right_pos();
}



static
bool
is_simple_indel_overlap(const std::vector<indel_info>& indel_buffer,
                        const unsigned size) {

    return (size==2 &&
            is_het_indel(indel_buffer[0].dindel) &&
            is_het_indel(indel_buffer[1].dindel));

}


static
void
get_hap_cigar(ALIGNPATH::path_t& apath,
              const indel_key& ik,
              const unsigned lead=1,
              const unsigned trail=0) {

    using namespace ALIGNPATH;

    apath.clear();
    if(lead) {
        apath.push_back(path_segment(MATCH,lead));
    }
    if(ik.delete_length()) {
        apath.push_back(path_segment(DELETE,ik.delete_length()));
    }
    if(ik.insert_length()) {
        apath.push_back(path_segment(INSERT,ik.insert_length()));
    }
    if(trail) {
        apath.push_back(path_segment(MATCH,trail));
    }
}


// figure out the per-site ploidy inside of indel based on each haplotype's match descriptor:
static
void
add_cigar_to_ploidy(const ALIGNPATH::path_t& apath,
                    std::vector<unsigned>& ploidy) {

    using namespace ALIGNPATH;
    const unsigned as(apath.size());
    int offset(-1);
    for(unsigned i(0);i<as;++i) {
        const path_segment& ps(apath[i]);
        if(ps.type==MATCH) {
            for(unsigned j(0);j<ps.length;++j){
                if(offset>=0) ploidy[offset]++;
                offset++;
            }
        } else if(ps.type==DELETE) {
            offset+=ps.length;
        }
    }
}




static
void
add_indel_modifiers(const starling_options& opt,
                    indel_info& ii) {

    ii.imod.gqx=std::min(ii.dindel.indel_qphred,ii.dindel.max_gt_qphred);
    if(opt.is_gvcf_min_gqx) {
        if(ii.imod.gqx<opt.gvcf_min_gqx) ii.imod.set_filter(VCF_FILTERS::LowGQX);
    }

    if(opt.is_gvcf_max_depth) {
        if(ii.isri.depth > opt.gvcf_max_depth) ii.imod.set_filter(VCF_FILTERS::HighDepth);
    }
}





// is the current site eligable to even be considered for block compression?
static
bool
is_site_record_blockable(const starling_options& opt,
                         const site_info& si) {

    if(si.dgt.is_snp) return false;

    if(si.ref!='N') {
        const double reffrac(static_cast<double>(si.known_counts[si.dgt.ref_gt]) /
                             static_cast<double>(si.n_used_calls));
        if(reffrac+opt.gvcf_block_max_nonref <= 1) return false;
    }
    return true;
}



// queue site record for writing, after
// possibly joining it into a compressed non-variant block
//
void
gvcf_aggregator::
queue_site_record(const site_info& si) {

    if(! is_site_record_blockable(_opt,si)) {
        write_block_site_record();
        write_site_record(si);
    }

    if(! _block.test(si)) {
        write_block_site_record();
    }
    
    _block.join(si);
}



static
void
print_vcf_alt(const unsigned gt,
              const unsigned ref_gt,
              std::ostream& os) {

    bool is_print(false);
    for(unsigned b(0);b<N_BASE;++b){
        if(b==ref_gt) continue;
        if(DIGT::expect2(b,gt)) {
            if(is_print) os << ',';
            os << id_to_base(b);
            is_print=true;
        }
    }
    if(! is_print) os << '.';
}



void
gvcf_aggregator::
write_site_record(const site_info& si) const {

    std::ostream& os(*_osptr);

    os << _chrom << '\t'  // CHROM
       << (si.pos+1) << '\t'  // POS
       << ".\t"           // ID
       << si.ref << '\t'; // REF

    // ALT
    if(si.smod.is_unknown || si.smod.is_block) {
        os << '.';
    } else {
        print_vcf_alt(si.smod.max_gt,si.dgt.ref_gt,os);
    }
    os << '\t';

    // QUAL:
    if(si.smod.is_qual()) {
        os << si.dgt.genome.snp_qphred;
    } else {
        os << '.';
    }
    os << '\t';

    // FILTER:
    si.smod.write_filters(os);
    os << '\t';

    // INFO:
    if(si.smod.is_block) {
        os << "END=" << (si.pos+_block.count) << ';';
        os << _opt.gvcf_block_label;
    } else {
        os << '.';
    }
    os << '\t';

    //FORMAT
    os << "GT:GQX" << '\t';

    //SAMPLE
    os << si.get_gt() << ';';
    if(si.smod.is_gqx()) {
        if(si.smod.is_block) {
            os << _block.block_gqx.min();
        } else {
            os << si.smod.gqx;
        }
    } else {
        os << '.';
    }
    os << '\n';
}


// set the CIGAR string:
void
gvcf_aggregator::
modify_single_indel_record() {
    assert(_indel_buffer_size==1);

    indel_info& ii(_indel_buffer[0]);
    get_hap_cigar(ii.imod.cigar,ii.ik);

    add_indel_modifiers(_opt,ii);
}



static
void
modify_indel_overlap_site(const starling_options& opt,
                          const indel_info& ii,
                          const unsigned ploidy,
                          site_info& si) {

    // inherit any filters from the indel:
    si.smod.filters &= ii.imod.filters;

    // change ploidy:
    if(ploidy==1) {
        // limit qual and gq values to those of the indel
        si.dgt.genome.snp_qphred = std::min(si.dgt.genome.snp_qphred,ii.dindel.indel_qphred);
        si.smod.gqx = std::min(si.smod.gqx,ii.dindel.max_gt_qphred);

        if(DIGT::is_het(si.smod.max_gt)) {
            si.smod.set_filter(VCF_FILTERS::SiteConflict);
            si.smod.modified_gt=MODIFIED_SITE_GT::UNKNOWN;
        } else {
            if(si.smod.max_gt == si.dgt.ref_gt) {
                si.smod.modified_gt=MODIFIED_SITE_GT::ZERO;
            } else {
                si.smod.modified_gt=MODIFIED_SITE_GT::ONE;
            }
        }
    } else if(ploidy==0) {
        si.smod.modified_gt=MODIFIED_SITE_GT::UNKNOWN;
        si.smod.is_zero_ploidy=true;
    } else {
        assert(0);
    }

    // after all those changes we need to rerun the site filters:
    set_site_filters(opt,si);
}



static
void
modify_indel_conflict_site(site_info& si) {

    si.smod.set_filter(VCF_FILTERS::IndelConflict);
}



void
gvcf_aggregator::
modify_overlap_indel_record() {
    
    // can only handle simple 2-indel overlaps right now:
    assert(_indel_buffer_size==2);

    // accumutate all modification info in the *first* indel record:
    indel_info& ii(_indel_buffer[0]);

    ii.imod.is_overlap=true;

    // there's going to be 1 (possibly empty) fill range in front of one haplotype
    // and one possibly empty fill range on the back of one haplotype
    std::string leading_seq,trailing_seq;

    const pos_t indel_begin_pos(ii.pos-1);

    // add shared information (to the first indel only)
    // make extended vcf ref seq:
    _ref.get_substring(indel_begin_pos,(_indel_end_pos-indel_begin_pos),ii.iri.vcf_ref_seq);

    ii.imod.ploidy.resize(_indel_end_pos-ii.pos,0);

    // add per-haplotype information:
    for(unsigned hap(0);hap<2;++hap) {
        //reduce qual and gt to the lowest of the set:
        if(hap) {
            if(ii.dindel.indel_qphred>_indel_buffer[hap].dindel.indel_qphred) {
                ii.dindel.indel_qphred = _indel_buffer[hap].dindel.indel_qphred;
            }
            if(ii.dindel.max_gt_qphred>_indel_buffer[hap].dindel.max_gt_qphred) {
                ii.dindel.max_gt_qphred = _indel_buffer[hap].dindel.max_gt_qphred;
            }
        }
        
        // extend leading sequence start back 1 for vcf compat, and end back 1 to concat with vcf_indel_seq
        _ref.get_substring(indel_begin_pos,(_indel_buffer[hap].pos-indel_begin_pos)-1,leading_seq);
        const unsigned trail_len(_indel_end_pos-_indel_buffer[hap].ik.right_pos());
        _ref.get_substring(_indel_end_pos-trail_len,trail_len,trailing_seq);


        _indel_buffer[hap].iri.vcf_indel_seq = leading_seq + _indel_buffer[hap].iri.vcf_indel_seq + trailing_seq;

        get_hap_cigar(_indel_buffer[hap].imod.cigar,
                      _indel_buffer[hap].ik,
                      leading_seq.size()+1,
                      trailing_seq.size());

        // add to the ploidy object:
        add_cigar_to_ploidy(_indel_buffer[hap].imod.cigar,ii.imod.ploidy);
    }

    add_indel_modifiers(_opt,ii);
}



// set the CIGAR string:
void
gvcf_aggregator::
modify_conflict_indel_record() {
    assert(_indel_buffer_size>1);

    for(unsigned i(0);i<_indel_buffer_size;++i) {
        indel_info& ii(_indel_buffer[i]);
        get_hap_cigar(ii.imod.cigar,ii.ik);

        ii.imod.set_filter(VCF_FILTERS::IndelConflict);

        add_indel_modifiers(_opt,ii);
    }
}



void
gvcf_aggregator::
write_indel_record(const unsigned write_index) {
    
    assert(_indel_buffer_size>0);

    // flush any non-variant block before starting:
    write_block_site_record();

    std::ostream& os(*_osptr);
    indel_info& ii(_indel_buffer[write_index]);

    os << _chrom << '\t'   // CHROM
       << ii.pos << '\t'   // POS
       << ".\t"            // ID
       << ii.iri.vcf_ref_seq << '\t'; // REF

    // ALT
    unsigned end_index(write_index);
    if(ii.imod.is_overlap) {
        end_index++;
    }
        
    for(unsigned i(write_index);i<=end_index;++i) {
        if(i!=write_index) os << ',';
        os << _indel_buffer[i].iri.vcf_indel_seq;
    }
    os << '\t';

    os << ii.dindel.indel_qphred << '\t'; //QUAL

    // FILTER:
    ii.imod.write_filters(os);
    os << '\t';

    // INFO
    os << "CIGAR=";
    for(unsigned i(write_index);i<=end_index;++i) {
        if(i!=write_index) os << ',';
        os << _indel_buffer[i].imod.cigar;
    }
    os << '\t';

    //FORMAT
    os << "GT:GQX" << '\t';

    //SAMPLE
    os << ii.get_gt() << ':' << ii.imod.gqx  << '\n';
}



void
gvcf_aggregator::
process_overlaps() {

    if(0==_indel_buffer_size) return;
    
    bool is_conflict_print(false);

    // do the overlap processing:
    if(_indel_buffer_size==1) {
        // simple case of no overlap:
        modify_single_indel_record();
    } else {
        if(is_simple_indel_overlap(_indel_buffer,_indel_buffer_size)){
            // handle the simplest possible overlap case (two hets):
            modify_overlap_indel_record();
        } else {
            // mark the whole region as conflicting
            modify_conflict_indel_record();
            is_conflict_print=true;
        }
    }

    *_osptr << "INDEL_SIZE: " << _indel_buffer_size << "\n";

    // process sites to be consistent with overlapping indels:
    for(unsigned i(0);i<_site_buffer_size;++i) {
        const pos_t offset(_site_buffer[i].pos-_indel_buffer[0].pos);
        assert(offset>=0);
        if(! is_conflict_print) {
            modify_indel_overlap_site(_opt,_indel_buffer[0],_indel_buffer[0].get_ploidy(offset),_site_buffer[i]);
        } else {
            modify_indel_conflict_site(_site_buffer[i]);
        }
    }


    unsigned indel_index(0);
    unsigned site_index(0);

    while(true) {
        const bool is_indel(indel_index<_indel_buffer_size);
        const bool is_site(site_index<_site_buffer_size);
        if(! (is_indel || is_site)) break;

        if(is_indel && ((! is_site) || _indel_buffer[indel_index].pos <= _site_buffer[site_index].pos)) {
            // print indel:
            write_indel_record(indel_index);
            if(is_conflict_print) {
                indel_index++;
            } else {
                indel_index=_indel_buffer_size;
            }
        } else {
            // print site:
            queue_site_record(_site_buffer[site_index]);
            site_index++;
        }
    }

    _indel_buffer_size = 0;
    _site_buffer_size = 0;
}



