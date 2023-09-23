//Copyright 2017 Ryan Wick

//This file is part of Bandage.

//Bandage is free software: you can redistribute it and/or modify
//it under the terms of the GNU General Public License as published by
//the Free Software Foundation, either version 3 of the License, or
//(at your option) any later version.

//Bandage is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.

//You should have received a copy of the GNU General Public License
//along with Bandage.  If not, see <http://www.gnu.org/licenses/>.


#include "querypath.h"
#include "query.h"

#include "graph/debruijnnode.h"
#include "graph/assemblygraph.h"
#include "graph/debruijnedge.h"

#include "program/globals.h"

#include <limits>

using namespace search;

QueryPath::QueryPath(Path path, Query *query)
        : m_path(std::move(path)), m_query(query) {
    //This function follows the path, returning the BLAST hits it finds for the
    //query.  It requires that the hits occur in order, i.e. that each hit in
    //the path begins later in the query than the previous hit.

    const Hit *previousHit = nullptr;
    const auto &pathNodes = m_path.nodes();
    for (int i = 0; i < pathNodes.size(); ++i) {
        DeBruijnNode * node = pathNodes[i];

        Query::Hits hitsThisNode;
        for (auto &queryHit : query->getHits()) {
            if (queryHit->m_node == node)
                hitsThisNode.push_back(queryHit.get());
        }

        std::sort(hitsThisNode.begin(), hitsThisNode.end(),
                  [](const Hit *a, const Hit *b) {
                      return a->m_queryStart < b->m_queryStart;
                  });

        for (auto *hit : hitsThisNode) {
            // First check to make sure the hits are within the path.  This means
            // if we are in the first or last nodes of the path, we need to make
            // sure that our hit is contained within the start/end positions.
            if ((i != 0 || hit->m_nodeStart >= m_path.getStartLocation().getPosition()) &&
                 (i != pathNodes.size() - 1 || hit->m_nodeEnd <= m_path.getEndLocation().getPosition())) {
                // Now make sure that the hit follows the previous hit in the
                // query.
                if (previousHit == nullptr || hit->m_queryStart > previousHit->m_queryStart) {
                    m_hits.push_back(hit);
                    previousHit = hit;
                }
            }
        }
    }
}

QueryPath::QueryPath(Path path, Query *query, std::vector<const Hit *> hits)
        : m_path(std::move(path)), m_query(query), m_hits(std::move(hits)) {}

double QueryPath::getMeanHitPercIdentity() const {
    int totalHitLength = 0;
    double sum = 0.0;

    for (const auto *hit : m_hits) {
        int hitLength = hit->m_alignmentLength;
        totalHitLength += hitLength;

        double hitIdentity = hit->m_percentIdentity;
        sum += hitIdentity * hitLength;
    }

    return totalHitLength == 0 ? 0.0 : sum / totalHitLength;
}

//This function looks at all of the hits in the path for this query and
//multiplies the e-values together. If the hits overlap each other, then
//this function reduces the e-values accoringly (effectively to prevent
//the overlapping region from being counted twice).
SciNot QueryPath::getEvalueProduct() const {
    double coefficientProduct = 1.0;
    int exponentSum = 0;

    for (int i = 0; i < m_hits.size(); ++i) {
        const Hit * thisHit = m_hits[i];
        SciNot thisHitEValue = thisHit->m_eValue;
        double eValueLenToRemove = 0.0;
        if (i > 0) {
            const Hit * previousHit = m_hits[i - 1];
            int overlap = getHitOverlap(previousHit, thisHit);
            if (overlap > 0)
                eValueLenToRemove += overlap / 2.0;
        }
        if (i < m_hits.size() - 1) {
            const Hit * nextHit = m_hits[i + 1];
            int overlap = getHitOverlap(thisHit, nextHit);
            if (overlap > 0)
                eValueLenToRemove += overlap / 2.0;
        }
        if (eValueLenToRemove > 0.0) {
            int thisHitLength = thisHit->getNodeLength();
            double reduction = (thisHitLength - eValueLenToRemove) / thisHitLength;
            thisHitEValue.power(reduction);
        }

        coefficientProduct *= thisHitEValue.getCoefficient();
        exponentSum += thisHitEValue.getExponent();
    }

    return SciNot(coefficientProduct, exponentSum);
}


int QueryPath::getHitOverlap(const Hit * hit1, const Hit * hit2) const {
    int hit1Start, hit1End, hit2Start, hit2End;
    QPair<DeBruijnNode *, DeBruijnNode *> possibleEdge(hit1->m_node, hit2->m_node);

    // Overlap in the same node is simple.
    if (hit1->m_node == hit2->m_node) {
        hit1Start = hit1->m_nodeStart - 1;
        hit1End = hit1->m_nodeEnd;
        hit2Start = hit2->m_nodeStart - 1;
        hit2End = hit2->m_nodeEnd;
    }

    // Overlap in connected nodes is a bit more complex - we need to express
    // the second hit's coordinates in terms of the first hit's node.
    else if (g_assemblyGraph->m_deBruijnGraphEdges.contains(possibleEdge)) {
        DeBruijnEdge * edge = g_assemblyGraph->m_deBruijnGraphEdges[possibleEdge];
        int overlap = edge->getOverlap();
        hit1Start = hit1->m_nodeStart;
        hit1End = hit1->m_nodeEnd;
        int hit1NodeLen = hit1->m_node->getLength();
        hit2Start = hit2->m_nodeStart + hit1NodeLen - overlap;
        hit2End = hit2->m_nodeEnd + hit1NodeLen - overlap;
    }
    else
        return 0;

    int overlap = std::min(hit1End, hit2End) - std::max(hit1Start, hit2Start);
    if (overlap > 0)
        return overlap;
    else
        return 0;
}


//This function looks at the length of the given path and compares it to how
//long the path should be for the hits it contains (i.e. if the path perfectly
//matched up the query).
double QueryPath::getRelativeLengthDiscrepancy() const {
    if (m_hits.empty())
        return std::numeric_limits<double>::max();

    int hitQueryLength = getHitQueryLength();

    int discrepancy = m_path.getLength() - hitQueryLength;
    return double(discrepancy) / hitQueryLength;
}


//This function gets the length of the path relative to the how long it should
//be.  A value of 1 means a perfect match; less than 1 means it is too short;
//more than 1 means it is too long.
double QueryPath::getRelativePathLength() const {
    return double(m_path.getLength()) / getHitQueryLength();
}


//This function gets the difference between how long the path is vs how long it
//should be.  A value of 0 means a perfect match; less than 0 means it is too
//short; more than 0 means it is too long.
int QueryPath::getAbsolutePathLengthDifference() const {
    return m_path.getLength() - getHitQueryLength();
}


QString QueryPath::getAbsolutePathLengthDifferenceString(bool commas) const {
    int lengthDisc = getAbsolutePathLengthDifference();
    QString lengthDiscSign = "";
    if (lengthDisc > 0)
        lengthDiscSign = "+";
    if (commas)
        return lengthDiscSign + formatIntForDisplay(lengthDisc);
    else
        return lengthDiscSign + QString::number(lengthDisc);
}

size_t QueryPath::queryLength() const { return m_query->getLength(); }

int QueryPath::queryStart() const {
    if (m_hits.empty())
        return -1;

    return m_hits.front()->m_queryStart;
}

int QueryPath::queryEnd() const {
    if (m_hits.empty())
        return -1;

    return m_hits.back()->m_queryEnd;
}

//This function returns the fraction of the query that is covered by the entire
//path.
double QueryPath::getPathQueryCoverage() const {
    if (m_hits.empty())
        return 0.0;

    int queryStart = m_hits.front()->m_queryStart;
    int queryEnd = m_hits.back()->m_queryEnd;
    int queryLength = m_query->getLength();

    int notIncluded = queryStart - 1;
    notIncluded += queryLength - queryEnd;

    return 1.0 - notIncluded / double(queryLength);
}


//This function returns the fraction of the query that is covered by hits in the
//path.
double QueryPath::getHitsQueryCoverage() const {
    return m_query->fractionCoveredByHits(m_hits);
}

//This function returns the length of the query which is covered by the path.
//It is returned in bp, whether or not the query is a protein or nucleotide
//sequence.
int QueryPath::getHitQueryLength() const {
    int queryStart = m_hits.front()->m_queryStart;
    int queryEnd = m_hits.back()->m_queryEnd;
    int hitQueryLength = queryEnd - queryStart + 1;

    if (m_query->getSequenceType() == PROTEIN)
        hitQueryLength *= 3;

    return hitQueryLength;
}

int QueryPath::getTotalHitMismatches() const {
    int total = 0;
    for (const auto *hit : m_hits)
        total += hit->m_numberMismatches;
    return total;
}

int QueryPath::getTotalHitGapOpens() const {
    int total = 0;
    for (const auto *hit : m_hits)
        total += hit->m_numberGapOpens;
    return total;
}


//This function is used for sorting the paths for a query from best to worst.
//it uses < to mean 'better than'.
bool QueryPath::operator<(QueryPath const &other) const {
    //First we compare using the E-value product.  This seems to value stronger
    //hits as well as paths with fewer, longer hits.
    SciNot aEValueProduct = getEvalueProduct();
    SciNot bEValueProduct = other.getEvalueProduct();
    if (aEValueProduct != bEValueProduct)
        return aEValueProduct < bEValueProduct;

    //If the code got here, then the two paths have the same e-value product,
    //possibly because they contain the same hits, or possibly because they both
    //contain hits so strong as to have an e-value of zero.

    //Now we compare using mean percent identity.
    double aMeanPercIdentity = getMeanHitPercIdentity();
    double bMeanPercIdentity = other.getMeanHitPercIdentity();
    if (aMeanPercIdentity != bMeanPercIdentity)
        return aMeanPercIdentity > bMeanPercIdentity;

    //Now we use the absolute value of the length discrepancy.
    double aLengthDiscrepancy = fabs(getRelativeLengthDiscrepancy());
    double bLengthDiscrepancy = fabs(other.getRelativeLengthDiscrepancy());
    if (aLengthDiscrepancy != bLengthDiscrepancy)
        return aLengthDiscrepancy < bLengthDiscrepancy;

    //Now we use fraction of query covered by hits.
    double aHitsQueryCoverage = getHitsQueryCoverage();
    double bHitsQueryCoverage = other.getHitsQueryCoverage();
    if (aHitsQueryCoverage != bHitsQueryCoverage)
        return aHitsQueryCoverage > bHitsQueryCoverage;

    return false;
}
